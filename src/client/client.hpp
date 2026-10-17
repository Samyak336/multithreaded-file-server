#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/net.hpp"
#include "common/protocol.hpp"

namespace mtfs {

struct RemoteFile {
    std::string name;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
};

// Blocking client for the mtfs protocol, used by mtfs-client, mtfs-stress
// and the end-to-end tests. Server-side failures are thrown as
// proto::StatusError carrying the ERR code; transport failures as NetError.
class Client {
public:
    Client(const std::string& host, std::uint16_t port);

    std::string login(const std::string& user, const std::string& password);  // returns the role
    std::string whoami();
    std::string help();
    std::vector<RemoteFile> list();
    RemoteFile stat(const std::string& name);

    std::string get(const std::string& name);  // whole file in memory
    std::uint64_t download(const std::string& name, const std::string& localPath);
    void put(const std::string& name, const std::string& data);
    std::uint64_t upload(const std::string& localPath, const std::string& name);
    void append(const std::string& name, const std::string& data);
    void copy(const std::string& source, const std::string& target);
    void rename(const std::string& source, const std::string& target);
    void remove(const std::string& name);
    void quit();

private:
    proto::Response readResponse();  // throws StatusError on ERR
    proto::Response request(const std::string& line);
    std::string expectOk(const std::string& line);
    std::uint64_t expectData(const std::string& line);
    void expectContinue(const std::string& line);

    Connection conn_;
};

}  // namespace mtfs
