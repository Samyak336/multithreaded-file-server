#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/fd.hpp"

namespace mtfs {

struct FileInfo {
    std::string name;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;  // seconds since the epoch
};

// A temporary file inside the storage directory. Uploads are written here
// first and renamed into place once complete; if that never happens, the
// destructor deletes the file.
class TempFile {
public:
    TempFile(UniqueFd fd, std::string path);
    TempFile(TempFile&& other) noexcept;
    TempFile& operator=(TempFile&&) = delete;
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    ~TempFile();

    int fd() const { return fd_.get(); }
    const std::string& path() const { return path_; }
    void markCommitted() { committed_ = true; }

private:
    UniqueFd fd_;
    std::string path_;
    bool committed_ = false;
};

// Flat file storage under one root directory. FileStore does no locking of
// its own: callers hold the LockManager locks for every name they touch.
// Failures are reported as proto::StatusError (404 missing, 409 already
// exists, 500 I/O error).
//
// Bytes already in a file are never overwritten in place. PUT and COPY write
// a new file and rename it over the old one; APPEND only adds bytes at the end.
class FileStore {
public:
    static constexpr std::size_t kMaxNameLength = 255;

    explicit FileStore(std::string root);

    // 1-255 characters from [A-Za-z0-9._-], not starting with '.'.
    // With no '/' allowed, "../../etc/passwd" cannot escape the root. With no
    // leading dot, clients can never see or touch the server's temp files.
    static bool isValidName(const std::string& name);

    const std::string& root() const { return root_; }
    bool exists(const std::string& name) const;
    std::vector<FileInfo> list() const;
    FileInfo info(const std::string& name) const;
    UniqueFd openForRead(const std::string& name, std::uint64_t& size) const;

    TempFile createTemp() const;
    void commit(TempFile& temp, const std::string& name);         // atomic create-or-replace
    void appendFrom(TempFile& temp, const std::string& name);     // all-or-nothing append
    void copy(const std::string& from, const std::string& to);    // 409 if `to` exists
    void rename(const std::string& from, const std::string& to);  // 409 if `to` exists
    void remove(const std::string& name);

private:
    std::string pathOf(const std::string& name) const { return root_ + "/" + name; }
    void removeStaleTemps();

    std::string root_;
};

}  // namespace mtfs
