#include "server/server.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>

#include "common/log.hpp"

namespace mtfs {

namespace {

constexpr int kListenBacklog = 128;

std::string peerName(const sockaddr_storage& addr) {
    char host[INET6_ADDRSTRLEN] = "?";
    std::uint16_t port = 0;
    if (addr.ss_family == AF_INET) {
        const auto* a = reinterpret_cast<const sockaddr_in*>(&addr);
        inet_ntop(AF_INET, &a->sin_addr, host, sizeof host);
        port = ntohs(a->sin_port);
    } else if (addr.ss_family == AF_INET6) {
        const auto* a = reinterpret_cast<const sockaddr_in6*>(&addr);
        inet_ntop(AF_INET6, &a->sin6_addr, host, sizeof host);
        port = ntohs(a->sin6_port);
    }
    return std::string(host) + ":" + std::to_string(port);
}

// Best effort and non-blocking: used for rejections, where the client may already be gone.
void sendLineQuietly(int fd, const std::string& line) {
    std::string framed = line + "\n";
    (void)!::send(fd, framed.data(), framed.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
}

}  // namespace

Server::Server(const ServerConfig& config)
    : config_(config),
      store_(config.root),
      context_{store_, locks_, config.usersFile, config.maxUploadBytes},
      listener_(listenTcp(config.host, config.port, kListenBacklog)),
      port_(localPort(listener_.get())),
      pool_(config.workers, config.queueCapacity) {
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) != 0) throw std::system_error(errno, std::generic_category(), "pipe2");
    wakeRead_.reset(fds[0]);
    wakeWrite_.reset(fds[1]);
    // Non-blocking listener: if a client resets between poll() and accept(),
    // accept() fails with EAGAIN instead of blocking the whole loop.
    ::fcntl(listener_.get(), F_SETFL, ::fcntl(listener_.get(), F_GETFL) | O_NONBLOCK);
}

Server::~Server() { stop(); }

void Server::run() {
    Log::info("listening on " + config_.host + ":" + std::to_string(port_) + " (" +
              std::to_string(pool_.workerCount()) + " workers, storage in " + store_.root() + ")");

    pollfd fds[2] = {{listener_.get(), POLLIN, 0}, {wakeRead_.get(), POLLIN, 0}};
    for (;;) {
        if (::poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "poll");
        }
        if (fds[1].revents != 0) break;  // stop() was called
        if ((fds[0].revents & POLLIN) == 0) continue;

        sockaddr_storage addr{};
        socklen_t len = sizeof addr;
        UniqueFd client(::accept4(listener_.get(), reinterpret_cast<sockaddr*>(&addr), &len, SOCK_CLOEXEC));
        if (!client.valid()) {
            if (errno == EMFILE || errno == ENFILE) {
                // Out of file descriptors: back off instead of spinning on poll().
                Log::warn("out of file descriptors; pausing accepts for 100 ms");
                ::usleep(100 * 1000);
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR && errno != ECONNABORTED) {
                Log::error(std::string("accept: ") + std::strerror(errno));
            }
            continue;
        }
        handOff(std::move(client), peerName(addr));
    }

    Log::info("shutting down; waiting for sessions to finish");
    listener_.reset();  // new connection attempts are refused from here on
    pool_.shutdown();   // queued sessions run (and see stopping_), then workers are joined
    Log::info("server stopped");
}

void Server::handOff(UniqueFd client, const std::string& peer) {
    // Limits how long an idle or stalled client can occupy a worker, and with
    // it any file lock that client's request holds.
    setSocketTimeouts(client.get(), config_.idleTimeoutSeconds);
    setNoDelay(client.get());
    int fd = client.release();
    // std::function needs a copyable callable, so the job carries the raw fd
    // and re-wraps it in a UniqueFd when it runs.
    if (!pool_.trySubmit([this, fd, peer] { serveClient(UniqueFd(fd), peer); })) {
        UniqueFd rejected(fd);
        Log::warn("rejecting " + peer + ": all workers busy and the queue is full");
        sendLineQuietly(rejected.get(), proto::errorLine(proto::Status::Unavailable, "server busy, try again later"));
    }
}

void Server::serveClient(UniqueFd client, const std::string& peer) {
    Connection conn(std::move(client));
    if (!registerClient(conn.fd())) {
        sendLineQuietly(conn.fd(), proto::errorLine(proto::Status::Unavailable, "server is shutting down"));
        return;
    }
    // Declared after `conn`, so it is destroyed first. The fd leaves the
    // registry before it is closed, and stop() can never shut down a reused
    // fd number that by then belongs to a different connection.
    struct Registration {
        Server& server;
        int fd;
        ~Registration() { server.unregisterClient(fd); }
    } registration{*this, conn.fd()};

    Log::info(peer + ": connected");
    Session session(conn, context_, peer);
    try {
        session.run();
    } catch (const TimeoutError&) {
        Log::info(peer + ": idle timeout");
        sendLineQuietly(conn.fd(), proto::errorLine(proto::Status::RequestTimeout, "idle timeout"));
    } catch (const ProtocolError& e) {
        Log::warn(peer + ": protocol error: " + e.what());
        sendLineQuietly(conn.fd(), proto::errorLine(proto::Status::BadRequest, e.what()));
    } catch (const NetError& e) {
        Log::info(peer + ": " + e.what());
    } catch (const std::exception& e) {
        Log::error(peer + ": session failed: " + e.what());
    }
    Log::info(peer + ": disconnected");
}

bool Server::registerClient(int fd) {
    MutexGuard g(clientsMutex_);
    if (stopping_) return false;
    clients_.insert(fd);
    return true;
}

void Server::unregisterClient(int fd) {
    MutexGuard g(clientsMutex_);
    clients_.erase(fd);
}

void Server::stop() {
    {
        MutexGuard g(clientsMutex_);
        if (stopping_) return;
        stopping_ = true;
        // Wake sessions blocked in recv() or send(); they see end-of-stream
        // or EPIPE and finish. Checking stopping_ and registering happen
        // under the same mutex, so no session can slip in afterwards.
        for (int fd : clients_) ::shutdown(fd, SHUT_RDWR);
    }
    char byte = 1;
    (void)!::write(wakeWrite_.get(), &byte, 1);
}

}  // namespace mtfs
