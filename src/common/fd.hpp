#pragma once

#include <cstddef>

namespace mtfs {

// Owns a file descriptor (socket or file) and closes it when destroyed,
// so no error path can leak one.
class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd() { reset(); }

    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    int get() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }
    int release() noexcept {
        int fd = fd_;
        fd_ = -1;
        return fd;
    }
    void reset(int fd = -1) noexcept;

private:
    int fd_ = -1;
};

// Blocking helpers for regular files. They retry on EINTR and throw
// std::system_error on failure.
std::size_t readSome(int fd, void* buf, std::size_t n);  // 0 means end of file
void writeFully(int fd, const void* data, std::size_t n);
void copyFd(int in, int out);  // copies until `in` reaches end of file

}  // namespace mtfs
