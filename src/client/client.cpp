#include "client/client.hpp"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace mtfs {

using proto::Response;

namespace {

// Rejects arguments that would break the one-line request format (or smuggle
// a second command in after a newline).
const std::string& word(const std::string& arg) {
    if (arg.empty()) throw std::invalid_argument("empty argument");
    for (char c : arg) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            throw std::invalid_argument("argument contains whitespace: '" + arg + "'");
    }
    return arg;
}

}  // namespace

Client::Client(const std::string& host, std::uint16_t port) : conn_(connectTcp(host, port)) {}

Response Client::readResponse() {
    std::string line;
    if (!conn_.readLine(line, proto::kMaxLineLength)) throw NetError("server closed the connection");
    Response r = proto::parseResponse(line);
    if (r.kind == Response::Kind::Error) throw proto::StatusError(static_cast<proto::Status>(r.code), r.text);
    return r;
}

Response Client::request(const std::string& line) {
    conn_.writeLine(line);
    return readResponse();
}

std::string Client::expectOk(const std::string& line) {
    Response r = request(line);
    if (r.kind != Response::Kind::Ok) throw ProtocolError("expected OK from the server");
    return r.text;
}

std::uint64_t Client::expectData(const std::string& line) {
    Response r = request(line);
    if (r.kind != Response::Kind::Data) throw ProtocolError("expected DATA from the server");
    return r.size;
}

void Client::expectContinue(const std::string& line) {
    // On ERR, request() throws before any payload is sent, so the stream stays in sync.
    Response r = request(line);
    if (r.kind != Response::Kind::Continue) throw ProtocolError("expected CONTINUE from the server");
}

std::string Client::login(const std::string& user, const std::string& password) {
    if (password.find_first_of("\r\n") != std::string::npos)
        throw std::invalid_argument("password contains a line break");
    return expectOk("LOGIN " + word(user) + " " + password);
}

std::string Client::whoami() { return expectOk("WHOAMI"); }

std::string Client::help() { return expectOk("HELP"); }

std::vector<RemoteFile> Client::list() {
    std::uint64_t size = expectData("LIST");
    std::string body(size, '\0');
    conn_.readExact(body.data(), body.size());

    std::vector<RemoteFile> files;
    std::size_t start = 0;
    while (start < body.size()) {
        std::size_t end = body.find('\n', start);
        if (end == std::string::npos) end = body.size();
        std::vector<std::string> f = proto::splitWords(body.substr(start, end - start));
        std::uint64_t fileSize = 0, mtime = 0;
        if (f.size() == 3 && proto::parseSize(f[1], fileSize) && proto::parseSize(f[2], mtime))
            files.push_back({f[0], fileSize, static_cast<std::int64_t>(mtime)});
        start = end + 1;
    }
    return files;
}

RemoteFile Client::stat(const std::string& name) {
    std::vector<std::string> f = proto::splitWords(expectOk("STAT " + word(name)));
    RemoteFile file{name, 0, 0};
    std::uint64_t mtime = 0;
    if (f.size() != 2 || !proto::parseSize(f[0], file.size) || !proto::parseSize(f[1], mtime))
        throw ProtocolError("malformed STAT reply");
    file.mtime = static_cast<std::int64_t>(mtime);
    return file;
}

std::string Client::get(const std::string& name) {
    std::uint64_t size = expectData("GET " + word(name));
    std::string data(size, '\0');
    conn_.readExact(data.data(), data.size());
    return data;
}

std::uint64_t Client::download(const std::string& name, const std::string& localPath) {
    // Write next to the target and rename on success, so a failed download
    // never truncates or clobbers an existing local file.
    std::string partial = localPath + ".part";
    UniqueFd out(::open(partial.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
    if (!out.valid()) throw std::system_error(errno, std::generic_category(), partial);
    try {
        std::uint64_t size = expectData("GET " + word(name));
        conn_.readToFd(out.get(), size);
        if (std::rename(partial.c_str(), localPath.c_str()) != 0)
            throw std::system_error(errno, std::generic_category(), localPath);
        return size;
    } catch (...) {
        ::unlink(partial.c_str());
        throw;
    }
}

void Client::put(const std::string& name, const std::string& data) {
    expectContinue("PUT " + word(name) + " " + std::to_string(data.size()));
    conn_.writeAll(data.data(), data.size());
    readResponse();
}

std::uint64_t Client::upload(const std::string& localPath, const std::string& name) {
    UniqueFd in(::open(localPath.c_str(), O_RDONLY | O_CLOEXEC));
    if (!in.valid()) throw std::system_error(errno, std::generic_category(), localPath);
    struct stat st {};
    if (::fstat(in.get(), &st) != 0) throw std::system_error(errno, std::generic_category(), localPath);
    if (!S_ISREG(st.st_mode)) throw std::invalid_argument(localPath + " is not a regular file");
    std::uint64_t size = static_cast<std::uint64_t>(st.st_size);

    expectContinue("PUT " + word(name) + " " + std::to_string(size));
    conn_.writeFromFd(in.get(), size);
    readResponse();
    return size;
}

void Client::append(const std::string& name, const std::string& data) {
    expectContinue("APPEND " + word(name) + " " + std::to_string(data.size()));
    conn_.writeAll(data.data(), data.size());
    readResponse();
}

void Client::copy(const std::string& source, const std::string& target) {
    expectOk("COPY " + word(source) + " " + word(target));
}

void Client::rename(const std::string& source, const std::string& target) {
    expectOk("RENAME " + word(source) + " " + word(target));
}

void Client::remove(const std::string& name) { expectOk("DELETE " + word(name)); }

void Client::quit() { expectOk("QUIT"); }

}  // namespace mtfs
