#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <pthread.h>
#include <vector>

#include "common/sync.hpp"

namespace mtfs {

// A fixed set of pthread workers fed by a bounded FIFO queue
// (producer-consumer). The accept loop produces jobs, the workers consume them.
//
// A fixed pool caps how many threads the server ever creates, no matter how
// many clients connect. Thread-per-connection would let a burst of
// connections create thousands of threads, each with its own stack.
class ThreadPool {
public:
    using Job = std::function<void()>;

    ThreadPool(std::size_t workers, std::size_t queueCapacity);
    ~ThreadPool();  // calls shutdown()
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Never blocks: returns false if the queue is full or the pool is closed,
    // so the caller can reject the work right away instead of stalling.
    bool trySubmit(Job job);
    // Stops accepting jobs, lets the workers finish everything already
    // queued, then joins them. Call from the owning thread; repeat calls are no-ops.
    void shutdown();

    std::size_t workerCount() const { return threads_.size(); }

private:
    static void* threadMain(void* arg);
    void workerLoop(std::size_t index);

    Mutex mutex_;  // protects queue_ and closed_
    CondVar hasWork_;
    std::deque<Job> queue_;
    std::size_t capacity_;
    bool closed_ = false;
    std::vector<pthread_t> threads_;
};

}  // namespace mtfs
