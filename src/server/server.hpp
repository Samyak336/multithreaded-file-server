#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>

#include "common/fd.hpp"
#include "common/protocol.hpp"
#include "common/sync.hpp"
#include "server/file_store.hpp"
#include "server/lock_manager.hpp"
#include "server/session.hpp"
#include "server/thread_pool.hpp"

namespace mtfs {

struct ServerConfig {
    std::string host = "0.0.0.0";
    std::uint16_t port = proto::kDefaultPort;  // 0 picks a free port (used by the tests)
    std::string root = "data/files";
    std::string usersFile = "data/users.db";
    std::size_t workers = 16;
    std::size_t queueCapacity = 64;
    std::uint64_t maxUploadBytes = 64ull << 20;  // 64 MiB
    int idleTimeoutSeconds = 300;
};

// The thread that calls run() is the acceptor. Each accepted connection
// becomes one job, and a pool worker serves that client from LOGIN to QUIT.
class Server {
public:
    explicit Server(const ServerConfig& config);  // binds and listens immediately
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    std::uint16_t port() const { return port_; }
    void run();   // accept loop; returns once stop() has been called and all sessions ended
    void stop();  // callable from any thread (but not from a signal handler)

private:
    void handOff(UniqueFd client, const std::string& peer);
    void serveClient(UniqueFd client, const std::string& peer);
    bool registerClient(int fd);
    void unregisterClient(int fd);

    // Declaration order is construction order: the pool comes last so its
    // workers start after everything they use exists, and it is destroyed
    // first, so they are joined before anything they use goes away.
    ServerConfig config_;
    FileStore store_;
    LockManager locks_;
    ServerContext context_;
    UniqueFd listener_;
    std::uint16_t port_ = 0;
    UniqueFd wakeRead_;   // self-pipe: stop() writes a byte here to wake
    UniqueFd wakeWrite_;  // the accept loop out of poll()
    Mutex clientsMutex_;
    std::set<int> clients_;  // sockets of the sessions in progress
    bool stopping_ = false;
    ThreadPool pool_;
};

}  // namespace mtfs
