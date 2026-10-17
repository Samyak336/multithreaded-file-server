#pragma once

// Thin RAII wrappers over pthread mutexes, condition variables and POSIX
// semaphores. The server uses these instead of <mutex> so that every
// synchronization step maps one-to-one onto the textbook primitive.

#include <cerrno>
#include <pthread.h>
#include <semaphore.h>
#include <system_error>

namespace mtfs {

class Mutex {
public:
    Mutex() { pthread_mutex_init(&m_, nullptr); }
    ~Mutex() { pthread_mutex_destroy(&m_); }
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void lock() { pthread_mutex_lock(&m_); }
    void unlock() { pthread_mutex_unlock(&m_); }
    pthread_mutex_t* native() { return &m_; }

private:
    pthread_mutex_t m_;
};

// Locks in the constructor and unlocks in the destructor, so an early
// return or an exception can never leave the mutex held.
class MutexGuard {
public:
    explicit MutexGuard(Mutex& m) : m_(m) { m_.lock(); }
    ~MutexGuard() { m_.unlock(); }
    MutexGuard(const MutexGuard&) = delete;
    MutexGuard& operator=(const MutexGuard&) = delete;

private:
    Mutex& m_;
};

class CondVar {
public:
    CondVar() { pthread_cond_init(&c_, nullptr); }
    ~CondVar() { pthread_cond_destroy(&c_); }
    CondVar(const CondVar&) = delete;
    CondVar& operator=(const CondVar&) = delete;

    // The caller must hold `m`. Wake-ups can be spurious, so always wait
    // in a loop that re-checks the condition.
    void wait(Mutex& m) { pthread_cond_wait(&c_, m.native()); }
    void signal() { pthread_cond_signal(&c_); }
    void broadcast() { pthread_cond_broadcast(&c_); }

private:
    pthread_cond_t c_;
};

// Counting semaphore (unnamed, shared between the threads of one process).
// Unlike a mutex, it may be released by a different thread than the one
// that acquired it, which the reader-writer lock relies on.
class Semaphore {
public:
    explicit Semaphore(unsigned initial) {
        if (sem_init(&s_, /*pshared=*/0, initial) != 0)
            throw std::system_error(errno, std::generic_category(), "sem_init");
    }
    ~Semaphore() { sem_destroy(&s_); }
    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;

    // P / down: block until the count is positive, then decrement it.
    void wait() {
        while (sem_wait(&s_) != 0) {
            if (errno != EINTR) throw std::system_error(errno, std::generic_category(), "sem_wait");
        }
    }
    // V / up: increment the count, waking one waiter if there is one.
    void post() { sem_post(&s_); }

private:
    sem_t s_;
};

}  // namespace mtfs
