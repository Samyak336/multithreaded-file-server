#include <filesystem>
#include <string>

#include "check.hpp"
#include "common/fd.hpp"
#include "common/protocol.hpp"
#include "server/file_store.hpp"

using namespace mtfs;
using proto::Status;

namespace {

template <typename F>
Status statusOf(F&& action) {
    try {
        action();
    } catch (const proto::StatusError& e) {
        return e.status();
    }
    return Status::Ok;
}

void put(FileStore& store, const std::string& name, const std::string& data) {
    TempFile temp = store.createTemp();
    writeFully(temp.fd(), data.data(), data.size());
    store.commit(temp, name);
}

void append(FileStore& store, const std::string& name, const std::string& data) {
    TempFile temp = store.createTemp();
    writeFully(temp.fd(), data.data(), data.size());
    store.appendFrom(temp, name);
}

std::size_t entriesIn(const std::string& dir) {
    std::size_t n = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        (void)entry;
        ++n;
    }
    return n;
}

void testNameValidation() {
    for (const char* ok : {"a", "notes.txt", "Report-2025_v2.pdf", "archive.tar.gz", "9"})
        CHECK(FileStore::isValidName(ok));
    for (const char* bad : {"", ".", "..", ".hidden", ".upload-abc123", "a/b", "../etc/passwd", "/etc/passwd",
                            "a b", "tab\there", "new\nline", "semi;colon", "star*"})
        CHECK(!FileStore::isValidName(bad));
    CHECK(FileStore::isValidName(std::string(255, 'a')));
    CHECK(!FileStore::isValidName(std::string(256, 'a')));
}

void testCommitAndAppend() {
    check::TempDir dir;
    FileStore store(dir.path);
    put(store, "greeting.txt", "hello");
    CHECK(check::readFile(dir.path + "/greeting.txt") == "hello");
    append(store, "greeting.txt", " world");
    CHECK(check::readFile(dir.path + "/greeting.txt") == "hello world");
    CHECK(store.info("greeting.txt").size == 11);

    put(store, "greeting.txt", "bye");  // replace
    CHECK(check::readFile(dir.path + "/greeting.txt") == "bye");

    // An upload that is never committed leaves nothing behind.
    {
        TempFile abandoned = store.createTemp();
        writeFully(abandoned.fd(), "junk", 4);
    }
    std::vector<FileInfo> files = store.list();
    CHECK(files.size() == 1 && files[0].name == "greeting.txt");
    CHECK(entriesIn(dir.path) == 1);

    CHECK(statusOf([&] { append(store, "missing.txt", "x"); }) == Status::NotFound);
}

void testCopyRenameRemove() {
    check::TempDir dir;
    FileStore store(dir.path);
    put(store, "a.txt", "alpha");
    put(store, "b.txt", "beta");

    store.copy("a.txt", "c.txt");
    CHECK(check::readFile(dir.path + "/c.txt") == "alpha");
    CHECK(statusOf([&] { store.copy("a.txt", "b.txt"); }) == Status::Conflict);  // never clobbers
    CHECK(statusOf([&] { store.copy("nope.txt", "d.txt"); }) == Status::NotFound);

    CHECK(statusOf([&] { store.rename("a.txt", "b.txt"); }) == Status::Conflict);
    CHECK(statusOf([&] { store.rename("nope.txt", "d.txt"); }) == Status::NotFound);
    store.rename("a.txt", "d.txt");
    CHECK(!store.exists("a.txt") && store.exists("d.txt"));

    store.remove("d.txt");
    CHECK(!store.exists("d.txt"));
    CHECK(statusOf([&] { store.remove("d.txt"); }) == Status::NotFound);
    CHECK(statusOf([&] { store.info("d.txt"); }) == Status::NotFound);
}

void testStaleTempsRemovedOnStartup() {
    check::TempDir dir;
    check::writeFile(dir.path + "/.upload-AbC123", "left over from a crash");
    check::writeFile(dir.path + "/keep.txt", "real file");
    FileStore store(dir.path);
    CHECK(!std::filesystem::exists(dir.path + "/.upload-AbC123"));
    CHECK(store.exists("keep.txt"));
}

}  // namespace

int main() {
    testNameValidation();
    testCommitAndAppend();
    testCopyRenameRemove();
    testStaleTempsRemovedOnStartup();
    return check::summary("test_file_store");
}
