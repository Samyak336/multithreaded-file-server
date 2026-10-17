#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/fd.hpp"

namespace mtfs {

// The transport failed (peer reset, connection closed mid-message). The
// connection cannot be used afterwards.
class NetError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A receive or send ran into the socket timeout (SO_RCVTIMEO/SO_SNDTIMEO).
class TimeoutError : public NetError {
public:
    using NetError::NetError;
};

// The peer broke the protocol, e.g. sent a header line longer than allowed.
class ProtocolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

UniqueFd listenTcp(const std::string& host, std::uint16_t port, int backlog);
UniqueFd connectTcp(const std::string& host, std::uint16_t port);
std::uint16_t localPort(int sockfd);  // the port actually bound (useful after binding port 0)
void setSocketTimeouts(int sockfd, int seconds);
// Disables Nagle's algorithm. The protocol often sends a short header and
// then a short payload as two writes; with Nagle on, the second write waits
// for the peer's delayed ACK, adding about 40 ms to the request.
void setNoDelay(int sockfd);

// A connected TCP stream plus an input buffer. Every message is a
// '\n'-terminated header line, optionally followed by a raw payload whose
// length the header announced, so the class offers both line reads and
// exact-length transfers. TCP is a byte stream: one recv() may return half
// a line or several lines, which is why reads go through the buffer.
class Connection {
public:
    explicit Connection(UniqueFd sock);

    int fd() const { return sock_.get(); }

    // Reads one line and strips the trailing "\n" or "\r\n". Returns false if
    // the peer closed the connection cleanly before sending any of it.
    // Throws ProtocolError if the line is longer than maxLength.
    bool readLine(std::string& line, std::size_t maxLength);
    void readExact(void* buf, std::size_t n);
    // Moves exactly n payload bytes from the socket into a file. If writing
    // the file fails, the rest of the payload is still read off the socket
    // (so the stream stays in sync) and std::system_error is thrown at the end.
    void readToFd(int fd, std::uint64_t n);

    void writeAll(const void* data, std::size_t n);
    void writeLine(const std::string& line);
    // Sends exactly n bytes read from a file.
    void writeFromFd(int fd, std::uint64_t n);

    // Shuts down both directions, waking any thread blocked on this socket.
    void shutdown();

private:
    bool fill();  // waits for more input; false at end of stream

    UniqueFd sock_;
    std::vector<char> buf_;
    std::size_t head_ = 0;  // unread input is buf_[head_, tail_)
    std::size_t tail_ = 0;
};

}  // namespace mtfs
