#pragma once

// The wire protocol shared by the server and the client.
//
// Requests are single '\n'-terminated text lines: VERB [ARG...].
// Responses:
//   OK [text]            success
//   DATA <size>          success, followed by exactly <size> raw bytes
//   CONTINUE             the server accepted an upload header; send the bytes now
//   ERR <code> <text>    failure, with HTTP-style status codes
//
// Uploads (PUT, APPEND) are two-phase: the client sends "PUT <name> <size>",
// the server checks the name, size and permissions and answers CONTINUE or
// ERR, and only after CONTINUE does the client send the <size> bytes. A
// rejected upload therefore never puts payload bytes on the wire that the
// server would misread as commands.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace mtfs::proto {

constexpr std::uint16_t kDefaultPort = 9090;
constexpr std::size_t kMaxLineLength = 4096;

enum class Status : int {
    Ok = 200,
    BadRequest = 400,
    Unauthorized = 401,
    Forbidden = 403,
    NotFound = 404,
    RequestTimeout = 408,
    Conflict = 409,
    PayloadTooLarge = 413,
    Internal = 500,
    Unavailable = 503,
};

// An error that becomes an "ERR <code> <text>" response. The server throws it
// from request handlers; the client throws it when it receives ERR.
class StatusError : public std::runtime_error {
public:
    StatusError(Status status, const std::string& message) : std::runtime_error(message), status_(status) {}
    Status status() const { return status_; }
    int code() const { return static_cast<int>(status_); }

private:
    Status status_;
};

struct Request {
    std::string verb;               // first word, upper-cased
    std::vector<std::string> args;  // the remaining words
    std::string rest;               // raw text after "VERB " (LOGIN uses it so passwords may contain spaces)
};

struct Response {
    enum class Kind { Ok, Data, Continue, Error };
    Kind kind = Kind::Ok;
    int code = 200;
    std::string text;        // OK or ERR message
    std::uint64_t size = 0;  // DATA payload size
};

std::vector<std::string> splitWords(const std::string& text);
// What follows the first n words of `text`, without the separating blanks.
std::string textAfterWords(const std::string& text, std::size_t n);
// Strict unsigned decimal: digits only, no sign, no overflow.
bool parseSize(const std::string& text, std::uint64_t& out);

Request parseRequest(const std::string& line);
Response parseResponse(const std::string& line);  // throws ProtocolError if malformed

std::string okLine(const std::string& text = "");
std::string dataLine(std::uint64_t size);
std::string continueLine();
std::string errorLine(Status status, const std::string& text);

}  // namespace mtfs::proto
