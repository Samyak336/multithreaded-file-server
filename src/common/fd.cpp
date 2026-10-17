#include "common/fd.hpp"

#include <cerrno>
#include <system_error>
#include <unistd.h>

namespace mtfs {

void UniqueFd::reset(int fd) noexcept {
    if (fd_ >= 0) ::close(fd_);
    fd_ = fd;
}

std::size_t readSome(int fd, void* buf, std::size_t n) {
    for (;;) {
        ssize_t got = ::read(fd, buf, n);
        if (got >= 0) return static_cast<std::size_t>(got);
        if (errno != EINTR) throw std::system_error(errno, std::generic_category(), "read");
    }
}

void writeFully(int fd, const void* data, std::size_t n) {
    const char* p = static_cast<const char*>(data);
    while (n > 0) {
        ssize_t put = ::write(fd, p, n);
        if (put < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "write");
        }
        p += put;
        n -= static_cast<std::size_t>(put);
    }
}

void copyFd(int in, int out) {
    char buf[64 * 1024];
    for (;;) {
        std::size_t got = readSome(in, buf, sizeof buf);
        if (got == 0) return;
        writeFully(out, buf, got);
    }
}

}  // namespace mtfs
