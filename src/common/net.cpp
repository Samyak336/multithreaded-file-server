#include "common/net.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <system_error>
#include <unistd.h>

namespace mtfs {

namespace {

constexpr std::size_t kBufferSize = 64 * 1024;

std::string errnoText(const char* what) { return std::string(what) + ": " + std::strerror(errno); }

struct AddrInfoDeleter {
    void operator()(addrinfo* list) const { freeaddrinfo(list); }
};
using AddrInfoList = std::unique_ptr<addrinfo, AddrInfoDeleter>;

AddrInfoList resolve(const std::string& host, std::uint16_t port, bool passive) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;  // IPv4 or IPv6, whichever the host resolves to
    hints.ai_socktype = SOCK_STREAM;
    if (passive) hints.ai_flags = AI_PASSIVE;
    addrinfo* list = nullptr;
    std::string service = std::to_string(port);
    int rc = getaddrinfo(host.empty() ? nullptr : host.c_str(), service.c_str(), &hints, &list);
    if (rc != 0) throw NetError("cannot resolve '" + host + "': " + gai_strerror(rc));
    return AddrInfoList(list);
}

}  // namespace

UniqueFd listenTcp(const std::string& host, std::uint16_t port, int backlog) {
    AddrInfoList addrs = resolve(host, port, /*passive=*/true);
    std::string lastError = "no usable address";
    for (addrinfo* ai = addrs.get(); ai != nullptr; ai = ai->ai_next) {
        UniqueFd fd(::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol));
        if (!fd.valid()) {
            lastError = errnoText("socket");
            continue;
        }
        // Lets a restarted server bind again while old connections are in TIME_WAIT.
        int on = 1;
        ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
        if (::bind(fd.get(), ai->ai_addr, ai->ai_addrlen) != 0) {
            lastError = errnoText("bind");
            continue;
        }
        if (::listen(fd.get(), backlog) != 0) {
            lastError = errnoText("listen");
            continue;
        }
        return fd;
    }
    throw NetError("cannot listen on " + host + ":" + std::to_string(port) + " (" + lastError + ")");
}

UniqueFd connectTcp(const std::string& host, std::uint16_t port) {
    AddrInfoList addrs = resolve(host, port, /*passive=*/false);
    std::string lastError = "no usable address";
    for (addrinfo* ai = addrs.get(); ai != nullptr; ai = ai->ai_next) {
        UniqueFd fd(::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol));
        if (!fd.valid()) {
            lastError = errnoText("socket");
            continue;
        }
        if (::connect(fd.get(), ai->ai_addr, ai->ai_addrlen) == 0) {
            setNoDelay(fd.get());
            return fd;
        }
        lastError = errnoText("connect");
    }
    throw NetError("cannot connect to " + host + ":" + std::to_string(port) + " (" + lastError + ")");
}

std::uint16_t localPort(int sockfd) {
    sockaddr_storage addr{};
    socklen_t len = sizeof addr;
    if (::getsockname(sockfd, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
        throw std::system_error(errno, std::generic_category(), "getsockname");
    if (addr.ss_family == AF_INET6) return ntohs(reinterpret_cast<sockaddr_in6*>(&addr)->sin6_port);
    return ntohs(reinterpret_cast<sockaddr_in*>(&addr)->sin_port);
}

void setSocketTimeouts(int sockfd, int seconds) {
    timeval tv{};
    tv.tv_sec = seconds;
    ::setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

void setNoDelay(int sockfd) {
    int on = 1;
    ::setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
}

Connection::Connection(UniqueFd sock) : sock_(std::move(sock)), buf_(kBufferSize) {}

bool Connection::fill() {
    // Only called once everything buffered has been consumed.
    head_ = tail_ = 0;
    for (;;) {
        ssize_t got = ::recv(sock_.get(), buf_.data(), buf_.size(), 0);
        if (got > 0) {
            tail_ = static_cast<std::size_t>(got);
            return true;
        }
        if (got == 0) return false;  // orderly shutdown by the peer
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) throw TimeoutError("receive timed out");
        throw NetError(errnoText("recv"));
    }
}

bool Connection::readLine(std::string& line, std::size_t maxLength) {
    line.clear();
    for (;;) {
        const char* begin = buf_.data() + head_;
        const char* end = buf_.data() + tail_;
        const char* newline = static_cast<const char*>(std::memchr(begin, '\n', end - begin));
        line.append(begin, newline != nullptr ? newline : end);
        if (line.size() > maxLength) throw ProtocolError("request line too long");
        if (newline != nullptr) {
            head_ = static_cast<std::size_t>(newline - buf_.data()) + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
        head_ = tail_;
        if (!fill()) {
            if (line.empty()) return false;
            throw NetError("connection closed in the middle of a line");
        }
    }
}

void Connection::readExact(void* buf, std::size_t n) {
    char* out = static_cast<char*>(buf);
    while (n > 0) {
        if (head_ == tail_ && !fill()) throw NetError("connection closed in the middle of a message");
        std::size_t chunk = std::min(n, tail_ - head_);
        std::memcpy(out, buf_.data() + head_, chunk);
        head_ += chunk;
        out += chunk;
        n -= chunk;
    }
}

void Connection::readToFd(int fd, std::uint64_t n) {
    int writeError = 0;
    while (n > 0) {
        if (head_ == tail_ && !fill()) throw NetError("connection closed in the middle of a transfer");
        std::size_t chunk = static_cast<std::size_t>(std::min<std::uint64_t>(n, tail_ - head_));
        if (writeError == 0) {
            try {
                writeFully(fd, buf_.data() + head_, chunk);
            } catch (const std::system_error& e) {
                writeError = e.code().value();  // keep draining; report once the payload is consumed
            }
        }
        head_ += chunk;
        n -= chunk;
    }
    if (writeError != 0) throw std::system_error(writeError, std::generic_category(), "write");
}

void Connection::writeAll(const void* data, std::size_t n) {
    const char* p = static_cast<const char*>(data);
    while (n > 0) {
        // MSG_NOSIGNAL: writing to a vanished peer returns EPIPE here instead
        // of raising SIGPIPE, whose default action would kill the server.
        ssize_t sent = ::send(sock_.get(), p, n, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) throw TimeoutError("send timed out");
            throw NetError(errnoText("send"));
        }
        p += sent;
        n -= static_cast<std::size_t>(sent);
    }
}

void Connection::writeLine(const std::string& line) {
    std::string framed = line;
    framed += '\n';
    writeAll(framed.data(), framed.size());
}

void Connection::writeFromFd(int fd, std::uint64_t n) {
    char chunk[64 * 1024];
    while (n > 0) {
        std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(n, sizeof chunk));
        std::size_t got = 0;
        try {
            got = readSome(fd, chunk, want);
        } catch (const std::system_error& e) {
            throw NetError(std::string("file read failed: ") + e.what());
        }
        // The header already promised n bytes, so a short file cannot be
        // reported in-band; dropping the connection is the only honest option.
        if (got == 0) throw NetError("file ended before the announced size");
        writeAll(chunk, got);
        n -= got;
    }
}

void Connection::shutdown() { ::shutdown(sock_.get(), SHUT_RDWR); }

}  // namespace mtfs
