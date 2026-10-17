#include "common/protocol.hpp"

#include <cctype>

#include "common/net.hpp"

namespace mtfs::proto {

namespace {

bool isBlank(char c) { return c == ' ' || c == '\t'; }

}  // namespace

std::vector<std::string> splitWords(const std::string& text) {
    std::vector<std::string> words;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && isBlank(text[i])) ++i;
        std::size_t start = i;
        while (i < text.size() && !isBlank(text[i])) ++i;
        if (i > start) words.push_back(text.substr(start, i - start));
    }
    return words;
}

std::string textAfterWords(const std::string& text, std::size_t n) {
    std::size_t i = 0;
    for (std::size_t word = 0; word < n; ++word) {
        while (i < text.size() && isBlank(text[i])) ++i;
        while (i < text.size() && !isBlank(text[i])) ++i;
    }
    while (i < text.size() && isBlank(text[i])) ++i;
    return text.substr(i);
}

bool parseSize(const std::string& text, std::uint64_t& out) {
    if (text.empty() || text.size() > 19) return false;  // 19 digits cannot overflow 64 bits
    std::uint64_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    out = value;
    return true;
}

Request parseRequest(const std::string& line) {
    Request req;
    std::size_t start = 0;
    while (start < line.size() && isBlank(line[start])) ++start;
    std::size_t end = start;
    while (end < line.size() && !isBlank(line[end])) ++end;

    req.verb = line.substr(start, end - start);
    for (char& c : req.verb) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (end < line.size()) req.rest = line.substr(end + 1);  // exactly one separator is consumed
    req.args = splitWords(req.rest);
    return req;
}

Response parseResponse(const std::string& line) {
    std::vector<std::string> words = splitWords(line);
    Response r;
    if (words.empty()) throw ProtocolError("empty response from server");

    if (words[0] == "OK") {
        r.kind = Response::Kind::Ok;
        r.text = textAfterWords(line, 1);
        return r;
    }
    if (words[0] == "CONTINUE" && words.size() == 1) {
        r.kind = Response::Kind::Continue;
        return r;
    }
    if (words[0] == "DATA" && words.size() == 2 && parseSize(words[1], r.size)) {
        r.kind = Response::Kind::Data;
        return r;
    }
    std::uint64_t code = 0;
    if (words[0] == "ERR" && words.size() >= 2 && parseSize(words[1], code) && code >= 100 && code <= 999) {
        r.kind = Response::Kind::Error;
        r.code = static_cast<int>(code);
        r.text = textAfterWords(line, 2);
        return r;
    }
    throw ProtocolError("malformed response from server: " + line);
}

std::string okLine(const std::string& text) { return text.empty() ? "OK" : "OK " + text; }

std::string dataLine(std::uint64_t size) { return "DATA " + std::to_string(size); }

std::string continueLine() { return "CONTINUE"; }

std::string errorLine(Status status, const std::string& text) {
    std::string line = "ERR " + std::to_string(static_cast<int>(status)) + " " + text;
    // A line break inside the message would split the response in two.
    for (char& c : line) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    return line;
}

}  // namespace mtfs::proto
