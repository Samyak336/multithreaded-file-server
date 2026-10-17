#pragma once

// A tiny test harness. CHECK records a failure and keeps going; each test
// binary returns non-zero if any check failed. watchdog() turns a deadlock
// into a test failure instead of a hung CI job.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace check {

inline std::atomic<int> failures{0};

inline void fail(const char* file, int line, const char* what) {
    std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", file, line, what);
    ++failures;
}

// SIGALRM's default action terminates the process, which ctest reports as a failure.
inline void watchdog(unsigned seconds) { alarm(seconds); }

inline int summary(const char* suite) {
    int n = failures.load();
    if (n == 0) {
        std::printf("%s: all checks passed\n", suite);
    } else {
        std::printf("%s: %d check(s) failed\n", suite, n);
    }
    return n == 0 ? 0 : 1;
}

// A fresh directory under /tmp, deleted with everything in it at scope exit.
struct TempDir {
    std::string path;
    TempDir() {
        char pattern[] = "/tmp/mtfs-test-XXXXXX";
        if (mkdtemp(pattern) == nullptr) {
            std::perror("mkdtemp");
            std::exit(1);
        }
        path = pattern;
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

inline std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

inline void writeFile(const std::string& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << data;
}

}  // namespace check

#define CHECK(cond)                                               \
    do {                                                          \
        if (!(cond)) check::fail(__FILE__, __LINE__, #cond);      \
    } while (0)

#define CHECK_THROWS(expr)                                                         \
    do {                                                                           \
        bool thrown_ = false;                                                      \
        try {                                                                      \
            (void)(expr);                                                          \
        } catch (...) {                                                            \
            thrown_ = true;                                                        \
        }                                                                          \
        if (!thrown_) check::fail(__FILE__, __LINE__, "expected an exception: " #expr); \
    } while (0)
