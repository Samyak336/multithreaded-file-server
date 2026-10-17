#include "server/file_store.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

#include "common/log.hpp"
#include "common/protocol.hpp"

namespace mtfs {

using proto::Status;
using proto::StatusError;

namespace {

constexpr char kTempPrefix[] = ".upload-";

[[noreturn]] void throwFor(int err, const std::string& name) {
    switch (err) {
        case ENOENT: throw StatusError(Status::NotFound, "no such file: " + name);
        case EEXIST: throw StatusError(Status::Conflict, "file already exists: " + name);
        case ENOSPC:
        case EDQUOT: throw StatusError(Status::Internal, "server storage is full");
        default: throw StatusError(Status::Internal, name + ": " + std::strerror(err));
    }
}

[[noreturn]] void throwNotRegular(const std::string& name) {
    throw StatusError(Status::NotFound, "not a regular file: " + name);
}

struct DirCloser {
    void operator()(DIR* dir) const { closedir(dir); }
};
using DirHandle = std::unique_ptr<DIR, DirCloser>;

bool isNameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
           c == '-';
}

}  // namespace

TempFile::TempFile(UniqueFd fd, std::string path) : fd_(std::move(fd)), path_(std::move(path)) {}

TempFile::TempFile(TempFile&& other) noexcept
    : fd_(std::move(other.fd_)), path_(std::move(other.path_)), committed_(other.committed_) {
    other.committed_ = true;  // the moved-from object must not delete the file
}

TempFile::~TempFile() {
    if (!committed_ && !path_.empty()) ::unlink(path_.c_str());
}

FileStore::FileStore(std::string root) : root_(std::move(root)) {
    while (root_.size() > 1 && root_.back() == '/') root_.pop_back();
    std::error_code ec;
    std::filesystem::create_directories(root_, ec);
    struct stat st {};
    if (::stat(root_.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
        throw std::runtime_error("storage root is not a usable directory: " + root_);
    removeStaleTemps();
}

bool FileStore::isValidName(const std::string& name) {
    if (name.empty() || name.size() > kMaxNameLength || name[0] == '.') return false;
    return std::all_of(name.begin(), name.end(), isNameChar);
}

bool FileStore::exists(const std::string& name) const {
    struct stat st {};
    return ::lstat(pathOf(name).c_str(), &st) == 0;
}

std::vector<FileInfo> FileStore::list() const {
    DirHandle dir(::opendir(root_.c_str()));
    if (!dir) throwFor(errno, root_);
    std::vector<FileInfo> files;
    while (dirent* entry = ::readdir(dir.get())) {
        std::string name = entry->d_name;
        if (!isValidName(name)) continue;  // skips ".", ".." and temp files
        struct stat st {};
        // A file may vanish between readdir() and lstat(); just leave it out.
        if (::lstat(pathOf(name).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        files.push_back({name, static_cast<std::uint64_t>(st.st_size), static_cast<std::int64_t>(st.st_mtime)});
    }
    std::sort(files.begin(), files.end(), [](const FileInfo& a, const FileInfo& b) { return a.name < b.name; });
    return files;
}

FileInfo FileStore::info(const std::string& name) const {
    struct stat st {};
    if (::lstat(pathOf(name).c_str(), &st) != 0) throwFor(errno, name);
    if (!S_ISREG(st.st_mode)) throwNotRegular(name);
    return {name, static_cast<std::uint64_t>(st.st_size), static_cast<std::int64_t>(st.st_mtime)};
}

UniqueFd FileStore::openForRead(const std::string& name, std::uint64_t& size) const {
    // O_NOFOLLOW: refuse a symlink planted in the storage directory.
    UniqueFd fd(::open(pathOf(name).c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (!fd.valid()) {
        if (errno == ELOOP) throwNotRegular(name);
        throwFor(errno, name);
    }
    struct stat st {};
    if (::fstat(fd.get(), &st) != 0) throwFor(errno, name);
    if (!S_ISREG(st.st_mode)) throwNotRegular(name);
    size = static_cast<std::uint64_t>(st.st_size);
    return fd;
}

TempFile FileStore::createTemp() const {
    // Same directory as the final file: rename() within one filesystem is atomic.
    std::string pattern = root_ + "/" + kTempPrefix + "XXXXXX";
    std::vector<char> path(pattern.begin(), pattern.end());
    path.push_back('\0');
    UniqueFd fd(::mkostemp(path.data(), O_CLOEXEC));
    if (!fd.valid()) throwFor(errno, "temporary file");
    ::fchmod(fd.get(), 0644);
    return TempFile(std::move(fd), std::string(path.data()));
}

void FileStore::commit(TempFile& temp, const std::string& name) {
    // rename() replaces the target atomically: anyone who opens the name sees
    // either the complete old file or the complete new one, never a mix.
    if (std::rename(temp.path().c_str(), pathOf(name).c_str()) != 0) throwFor(errno, name);
    temp.markCommitted();
}

void FileStore::appendFrom(TempFile& temp, const std::string& name) {
    UniqueFd out(::open(pathOf(name).c_str(), O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW));
    if (!out.valid()) {
        if (errno == ELOOP) throwNotRegular(name);
        throwFor(errno, name);
    }
    struct stat st {};
    if (::fstat(out.get(), &st) != 0) throwFor(errno, name);
    if (!S_ISREG(st.st_mode)) throwNotRegular(name);

    try {
        if (::lseek(temp.fd(), 0, SEEK_SET) < 0) throw std::system_error(errno, std::generic_category(), "lseek");
        copyFd(temp.fd(), out.get());
    } catch (const std::system_error& e) {
        // Roll back a partial append (e.g. the disk filled up) so the file is
        // left exactly as it was.
        if (::ftruncate(out.get(), st.st_size) != 0) Log::error("could not roll back a failed append to " + name);
        throwFor(e.code().value(), name);
    }
}

void FileStore::copy(const std::string& from, const std::string& to) {
    std::uint64_t size = 0;
    UniqueFd in = openForRead(from, size);
    if (exists(to)) throwFor(EEXIST, to);
    TempFile temp = createTemp();
    try {
        copyFd(in.get(), temp.fd());
    } catch (const std::system_error& e) {
        throwFor(e.code().value(), to);
    }
    commit(temp, to);
}

void FileStore::rename(const std::string& from, const std::string& to) {
    if (!exists(from)) throwFor(ENOENT, from);
    // Checking first is safe only because the caller holds write locks on
    // both names, so no other request can create `to` in between.
    if (exists(to)) throwFor(EEXIST, to);
    if (std::rename(pathOf(from).c_str(), pathOf(to).c_str()) != 0) throwFor(errno, from);
}

void FileStore::remove(const std::string& name) {
    struct stat st {};
    if (::lstat(pathOf(name).c_str(), &st) != 0) throwFor(errno, name);
    if (S_ISDIR(st.st_mode)) throwNotRegular(name);
    if (::unlink(pathOf(name).c_str()) != 0) throwFor(errno, name);
}

void FileStore::removeStaleTemps() {
    DirHandle dir(::opendir(root_.c_str()));
    if (!dir) return;
    int removed = 0;
    while (dirent* entry = ::readdir(dir.get())) {
        // Left behind if the server was killed in the middle of an upload.
        if (std::strncmp(entry->d_name, kTempPrefix, sizeof kTempPrefix - 1) == 0 &&
            ::unlink(pathOf(entry->d_name).c_str()) == 0) {
            ++removed;
        }
    }
    if (removed > 0) Log::info("removed " + std::to_string(removed) + " leftover temporary file(s) in " + root_);
}

}  // namespace mtfs
