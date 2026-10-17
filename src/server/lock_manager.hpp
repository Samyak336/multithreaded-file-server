#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/sync.hpp"
#include "server/rw_lock.hpp"

namespace mtfs {

enum class LockMode { Read, Write };

// One reader-writer lock per file name, created on first use and removed
// once nobody holds or waits for it, so the table stays as small as the set
// of files currently in use.
//
// Deadlock freedom. A deadlock needs a cycle of threads, each holding a lock
// the next one is waiting for (circular wait). Operations that need two files
// (COPY, RENAME) ask for both in a single acquireAll() call, which always
// locks them in ascending name order. When every thread acquires in the same
// global order, no thread can hold a "larger" name while waiting for a
// "smaller" one, so a cycle cannot form. The table mutex is held only for
// map lookups, never while blocking on a file lock, so it cannot be part of
// a cycle either.
class LockManager {
    struct Entry;

public:
    // RAII handle for the locks taken by one acquire()/acquireAll() call.
    // They are released when the guard is destroyed, on every path,
    // including exceptions and dropped connections.
    class Guard {
    public:
        Guard() = default;
        Guard(Guard&& other) noexcept;
        Guard& operator=(Guard&& other) noexcept;
        ~Guard() { release(); }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

        void release();

    private:
        friend class LockManager;
        struct Held {
            std::string name;
            LockMode mode;
            Entry* entry;
        };
        LockManager* owner_ = nullptr;
        std::vector<Held> held_;  // in acquisition order
    };

    LockManager() = default;
    LockManager(const LockManager&) = delete;
    LockManager& operator=(const LockManager&) = delete;

    Guard acquire(const std::string& name, LockMode mode);
    // Locks several files in ascending name order. Duplicate names are merged
    // into one lock (Write wins), so "RENAME a a" cannot deadlock on itself.
    Guard acquireAll(std::vector<std::pair<std::string, LockMode>> requests);

    std::size_t tableSize() const;  // number of live entries (used by tests)

private:
    struct Entry {
        RWLock lock;
        int users = 0;  // holders + waiters; the entry is erased when this drops to zero
    };

    Entry* pin(const std::string& name);
    void unpin(const std::string& name);
    void unlock(const Guard::Held& held);

    mutable Mutex tableMutex_;
    // Entries live on the heap and are pinned by `users`, so a thread can keep
    // using its Entry* after dropping the table mutex.
    std::unordered_map<std::string, std::unique_ptr<Entry>> table_;
};

}  // namespace mtfs
