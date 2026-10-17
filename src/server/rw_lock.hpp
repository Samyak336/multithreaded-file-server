#pragma once

#include "common/sync.hpp"

namespace mtfs {

// Reader-writer lock built from three semaphores: the starvation-free
// ("third") readers-writers solution.
//
// Any number of readers may hold the lock together; a writer holds it alone.
// In the simpler readers-preference version, a steady stream of overlapping
// readers keeps the reader count above zero forever and a writer never gets
// in. Here every reader and writer first passes through `turnstile_`. A
// writer keeps the turnstile while it waits for the current readers to
// drain, so readers that arrive after it queue behind it instead of
// overtaking it.
class RWLock {
public:
    RWLock() = default;
    RWLock(const RWLock&) = delete;
    RWLock& operator=(const RWLock&) = delete;

    void lockRead();
    void unlockRead();
    void lockWrite();
    void unlockWrite();

private:
    Semaphore turnstile_{1};   // arrival order: everyone passes through here one at a time
    Semaphore resource_{1};    // held by the active writer or by the active group of readers
    Semaphore countMutex_{1};  // protects readers_
    int readers_ = 0;
};

}  // namespace mtfs
