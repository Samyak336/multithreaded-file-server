#include "server/thread_pool.hpp"

#include <exception>
#include <memory>
#include <string>
#include <system_error>

#include "common/log.hpp"

namespace mtfs {

namespace {

struct WorkerStart {
    ThreadPool* pool;
    std::size_t index;
};

}  // namespace

ThreadPool::ThreadPool(std::size_t workers, std::size_t queueCapacity) : capacity_(queueCapacity) {
    threads_.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i) {
        auto* start = new WorkerStart{this, i};
        pthread_t tid;
        int rc = pthread_create(&tid, nullptr, &ThreadPool::threadMain, start);
        if (rc != 0) {
            delete start;
            shutdown();  // join the workers that did start before reporting the failure
            throw std::system_error(rc, std::generic_category(), "pthread_create");
        }
        threads_.push_back(tid);
    }
}

ThreadPool::~ThreadPool() { shutdown(); }

void* ThreadPool::threadMain(void* arg) {
    std::unique_ptr<WorkerStart> start(static_cast<WorkerStart*>(arg));
    start->pool->workerLoop(start->index);
    return nullptr;
}

bool ThreadPool::trySubmit(Job job) {
    {
        MutexGuard g(mutex_);
        if (closed_ || queue_.size() >= capacity_) return false;
        queue_.push_back(std::move(job));
    }
    hasWork_.signal();  // one new job needs one worker
    return true;
}

void ThreadPool::shutdown() {
    {
        MutexGuard g(mutex_);
        closed_ = true;
    }
    hasWork_.broadcast();  // wake every idle worker so each can see `closed_`
    for (pthread_t tid : threads_) pthread_join(tid, nullptr);
    threads_.clear();
}

void ThreadPool::workerLoop(std::size_t index) {
    std::string name = "worker-" + std::to_string(index);
    pthread_setname_np(pthread_self(), name.c_str());  // shows up in top -H and gdb
    Log::setThreadName(name);

    for (;;) {
        Job job;
        {
            MutexGuard g(mutex_);
            while (queue_.empty() && !closed_) hasWork_.wait(mutex_);
            if (queue_.empty()) return;  // closed and fully drained
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        // Run the job without holding the mutex, so other workers can take jobs meanwhile.
        try {
            job();
        } catch (const std::exception& e) {
            Log::error(std::string("job failed: ") + e.what());
        } catch (...) {
            Log::error("job failed with an unknown exception");
        }
    }
}

}  // namespace mtfs
