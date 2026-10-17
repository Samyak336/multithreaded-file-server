#pragma once

#include <string>

namespace mtfs {

// Minimal thread-safe logger. Each call writes one complete line to stderr:
//   2026-10-18 14:03:22.123 INFO  [worker-3] 127.0.0.1:53422: connected
class Log {
public:
    enum class Level { Debug = 0, Info, Warn, Error };

    static void setLevel(Level level);
    // Label shown on every line written by the calling thread.
    static void setThreadName(const std::string& name);

    static void debug(const std::string& msg) { write(Level::Debug, msg); }
    static void info(const std::string& msg) { write(Level::Info, msg); }
    static void warn(const std::string& msg) { write(Level::Warn, msg); }
    static void error(const std::string& msg) { write(Level::Error, msg); }

private:
    static void write(Level level, const std::string& msg);
};

}  // namespace mtfs
