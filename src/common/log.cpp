#include "common/log.hpp"

#include <atomic>
#include <cstdio>
#include <ctime>
#include <sys/time.h>

#include "common/sync.hpp"

namespace mtfs {

namespace {

std::atomic<int> g_level{static_cast<int>(Log::Level::Info)};
Mutex g_outputMutex;  // keeps lines from different threads from interleaving
thread_local std::string t_threadName = "thread";

const char* levelName(Log::Level level) {
    switch (level) {
        case Log::Level::Debug: return "DEBUG";
        case Log::Level::Info: return "INFO ";
        case Log::Level::Warn: return "WARN ";
        case Log::Level::Error: return "ERROR";
    }
    return "?    ";
}

}  // namespace

void Log::setLevel(Level level) { g_level.store(static_cast<int>(level)); }

void Log::setThreadName(const std::string& name) { t_threadName = name; }

void Log::write(Level level, const std::string& msg) {
    if (static_cast<int>(level) < g_level.load()) return;

    timeval now{};
    gettimeofday(&now, nullptr);
    tm local{};
    localtime_r(&now.tv_sec, &local);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &local);

    MutexGuard guard(g_outputMutex);
    std::fprintf(stderr, "%s.%03ld %s [%s] %s\n", stamp, static_cast<long>(now.tv_usec / 1000),
                 levelName(level), t_threadName.c_str(), msg.c_str());
}

}  // namespace mtfs
