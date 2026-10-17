#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "check.hpp"
#include "server/rw_lock.hpp"

using namespace mtfs;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

// Readers really share the lock: four of them must be inside at the same time.
void testReadersShare() {
    RWLock lock;
    std::atomic<int> inside{0};
    std::atomic<bool> allInside{false};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            lock.lockRead();
            ++inside;
            Clock::time_point until = Clock::now() + 5s;
            while (inside.load() < 4 && Clock::now() < until) std::this_thread::yield();
            if (inside.load() == 4) allInside = true;
            lock.unlockRead();
        });
    }
    for (std::thread& t : readers) t.join();
    CHECK(allInside.load());
}

// A writer excludes everyone: no reader and no second writer may be inside
// at the same time. `counter` is a plain variable protected only by the
// lock, so ThreadSanitizer would also report a data race if exclusion broke.
void testWriterExclusive() {
    RWLock lock;
    std::atomic<int> readers{0}, writers{0};
    std::atomic<bool> violated{false};
    long long counter = 0;
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 20000; ++i) {
                if ((i + t) % 4 == 0) {
                    lock.lockWrite();
                    if (++writers != 1 || readers.load() != 0) violated = true;
                    ++counter;
                    --writers;
                    lock.unlockWrite();
                } else {
                    lock.lockRead();
                    ++readers;
                    if (writers.load() != 0) violated = true;
                    --readers;
                    lock.unlockRead();
                }
            }
        });
    }
    for (std::thread& t : threads) t.join();
    CHECK(!violated.load());
    CHECK(counter == 8 * 5000);
}

// Fairness: overlapping readers keep the lock busy non-stop. With a
// readers-preference lock the writer could wait forever; here it only waits
// for the readers that are already inside.
void testWriterNotStarved() {
    RWLock lock;
    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            while (!stop.load()) {
                lock.lockRead();
                std::this_thread::sleep_for(2ms);
                lock.unlockRead();
            }
        });
    }
    std::this_thread::sleep_for(50ms);
    Clock::time_point start = Clock::now();
    lock.lockWrite();
    Clock::duration waited = Clock::now() - start;
    lock.unlockWrite();
    stop = true;
    for (std::thread& t : readers) t.join();
    CHECK(waited < 1s);
}

}  // namespace

int main() {
    check::watchdog(120);
    testReadersShare();
    testWriterExclusive();
    testWriterNotStarved();
    return check::summary("test_rw_lock");
}
