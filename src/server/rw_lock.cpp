#include "server/rw_lock.hpp"

namespace mtfs {

void RWLock::lockRead() {
    turnstile_.wait();  // blocks while a writer that arrived earlier is waiting or writing
    countMutex_.wait();
    // The first reader takes `resource_` on behalf of the whole group. The
    // last reader to leave releases it, and that is usually a different
    // thread, which is why `resource_` has to be a semaphore: a pthread
    // mutex may only be unlocked by the thread that locked it.
    if (++readers_ == 1) resource_.wait();
    countMutex_.post();
    turnstile_.post();  // let the next arrival (reader or writer) through
}

void RWLock::unlockRead() {
    countMutex_.wait();
    if (--readers_ == 0) resource_.post();  // the last reader lets a waiting writer in
    countMutex_.post();
}

void RWLock::lockWrite() {
    turnstile_.wait();  // claim our place in line; readers arriving later now wait behind us
    resource_.wait();   // wait for the active readers (or writer) to finish
    turnstile_.post();
}

void RWLock::unlockWrite() { resource_.post(); }

}  // namespace mtfs
