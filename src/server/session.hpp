#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "auth/role.hpp"
#include "common/net.hpp"
#include "common/protocol.hpp"

namespace mtfs {

class FileStore;
class LockManager;

// Shared by every session: the storage, the lock table and the limits.
struct ServerContext {
    FileStore& store;
    LockManager& locks;
    std::string usersFile;
    std::uint64_t maxUploadBytes;
};

// One client connection, from LOGIN to QUIT, served on a single worker thread.
class Session {
public:
    Session(Connection& conn, ServerContext& ctx, std::string peer);

    // Serves requests until the client quits or disconnects. Request errors
    // become ERR responses; transport errors (NetError) propagate to the caller.
    void run();

private:
    using Args = std::vector<std::string>;
    struct Command;

    void dispatch(const proto::Request& req);
    void handleLogin(const proto::Request& req);
    void doList(const Args& args);
    void doStat(const Args& args);
    void doGet(const Args& args);
    void doPut(const Args& args);
    void doAppend(const Args& args);
    void doCopy(const Args& args);
    void doRename(const Args& args);
    void doDelete(const Args& args);

    std::uint64_t parseUploadSize(const std::string& text) const;
    void reply(const std::string& line) { conn_.writeLine(line); }
    std::string who() const;

    Connection& conn_;
    ServerContext& ctx_;
    std::string peer_;
    std::optional<std::string> user_;
    Role role_ = Role::Reader;
    int failedLogins_ = 0;
    bool done_ = false;
};

}  // namespace mtfs
