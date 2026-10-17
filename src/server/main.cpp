#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <pthread.h>
#include <string>

#include "auth/user_store.hpp"
#include "common/log.hpp"
#include "server/server.hpp"

using namespace mtfs;

namespace {

void usage(const char* program) {
    std::cerr << "usage: " << program << " [options]\n"
              << "  --host ADDR          address to bind (default 0.0.0.0)\n"
              << "  --port N             TCP port (default 9090)\n"
              << "  --root DIR           storage directory (default data/files)\n"
              << "  --users FILE         user database (default data/users.db)\n"
              << "  --workers N          worker threads, i.e. concurrent sessions (default 16)\n"
              << "  --queue N            connections that may wait for a worker (default 64)\n"
              << "  --max-upload-mb N    largest accepted upload (default 64)\n"
              << "  --idle-timeout SEC   disconnect idle clients after SEC seconds (default 300)\n"
              << "  --verbose            log every request\n";
}

unsigned long parseNumber(const std::string& flag, const char* text, unsigned long min, unsigned long max) {
    char* end = nullptr;
    errno = 0;
    unsigned long value = std::strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < min || value > max) {
        std::cerr << "invalid value for " << flag << ": " << text << "\n";
        std::exit(2);
    }
    return value;
}

struct SignalWaiter {
    Server* server = nullptr;
    sigset_t signals;
};

// SIGINT and SIGTERM are blocked in every thread and collected here with
// sigwait(). Shutdown then runs as ordinary code on an ordinary thread; inside
// an asynchronous signal handler, locking a mutex or logging would be unsafe.
void* signalThread(void* arg) {
    auto* waiter = static_cast<SignalWaiter*>(arg);
    Log::setThreadName("signals");
    int sig = 0;
    if (sigwait(&waiter->signals, &sig) == 0 && sig != SIGUSR1)
        Log::info(std::string("received ") + strsignal(sig) + ", stopping");
    waiter->server->stop();
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    ServerConfig config;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        std::string flag = argv[i];
        auto value = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::cerr << flag << " needs a value\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (flag == "--host") config.host = value();
        else if (flag == "--port") config.port = static_cast<std::uint16_t>(parseNumber(flag, value(), 0, 65535));
        else if (flag == "--root") config.root = value();
        else if (flag == "--users") config.usersFile = value();
        else if (flag == "--workers") config.workers = parseNumber(flag, value(), 1, 1024);
        else if (flag == "--queue") config.queueCapacity = parseNumber(flag, value(), 1, 65536);
        else if (flag == "--max-upload-mb") config.maxUploadBytes = parseNumber(flag, value(), 1, 1u << 20) << 20;
        else if (flag == "--idle-timeout") config.idleTimeoutSeconds = static_cast<int>(parseNumber(flag, value(), 1, 86400));
        else if (flag == "--verbose") verbose = true;
        else if (flag == "--help" || flag == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            std::cerr << "unknown option " << flag << "\n";
            usage(argv[0]);
            return 2;
        }
    }
    Log::setThreadName("main");
    if (verbose) Log::setLevel(Log::Level::Debug);

    // Block the shutdown signals before any thread exists; every thread
    // created afterwards inherits the mask. SIGUSR1 is only used to release
    // the signal thread at the end.
    SignalWaiter waiter;
    sigemptyset(&waiter.signals);
    sigaddset(&waiter.signals, SIGINT);
    sigaddset(&waiter.signals, SIGTERM);
    sigaddset(&waiter.signals, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &waiter.signals, nullptr);
    std::signal(SIGPIPE, SIG_IGN);  // backup for MSG_NOSIGNAL

    try {
        UserStore users(config.usersFile);
        users.load();  // fail early on a malformed database
        if (users.empty())
            Log::warn("no users in " + config.usersFile + "; create one with: mtfs-user --db " + config.usersFile +
                      " add <name> admin");

        Server server(config);
        waiter.server = &server;
        pthread_t signals;
        if (pthread_create(&signals, nullptr, signalThread, &waiter) != 0)
            throw std::runtime_error("cannot start the signal thread");

        int status = 0;
        try {
            server.run();
        } catch (const std::exception& e) {
            Log::error(e.what());
            status = 1;
        }
        pthread_kill(signals, SIGUSR1);  // wake the signal thread if no real signal arrived
        pthread_join(signals, nullptr);
        return status;
    } catch (const std::exception& e) {
        Log::error(e.what());
        return 1;
    }
}
