#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "check.hpp"
#include "server/lock_manager.hpp"

using namespace mtfs;
using namespace std::chrono_literals;

namespace {

// Two files locked in opposite orders by many threads at once: the classic
// deadlock recipe. acquireAll() sorts the names, so this has to finish (if it
// didn't, the watchdog would kill the test).
void testOppositeOrderDoesNotDeadlock() {
    LockManager locks;
    long long counter = 0;  // protected by the two write locks
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 5000; ++i) {
                bool forward = (t + i) % 2 == 0;
                LockManager::Guard guard =
                    forward ? locks.acquireAll({{"alpha", LockMode::Write}, {"beta", LockMode::Write}})
                            : locks.acquireAll({{"beta", LockMode::Write}, {"alpha", LockMode::Write}});
                ++counter;
            }
        });
    }
    for (std::thread& t : threads) t.join();
    CHECK(counter == 8 * 5000);
    CHECK(locks.tableSize() == 0);  // every entry was cleaned up
}

// Mixed modes in opposite orders: COPY a b (read a, write b) racing COPY b a.
void testMixedModesDoNotDeadlock() {
    LockManager locks;
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 5000; ++i) {
                if ((t + i) % 2 == 0) {
                    LockManager::Guard g = locks.acquireAll({{"a", LockMode::Read}, {"b", LockMode::Write}});
                } else {
                    LockManager::Guard g = locks.acquireAll({{"b", LockMode::Read}, {"a", LockMode::Write}});
                }
            }
        });
    }
    for (std::thread& t : threads) t.join();
    CHECK(locks.tableSize() == 0);
}

// The same name twice (RENAME a a) must not wait for itself, and the merged
// lock must be the stronger mode: a reader on another thread has to wait.
void testDuplicateNamesAreMerged() {
    LockManager locks;
    LockManager::Guard guard = locks.acquireAll({{"same", LockMode::Read}, {"same", LockMode::Write}});
    CHECK(locks.tableSize() == 1);

    std::atomic<bool> readerIn{false};
    std::thread reader([&] {
        LockManager::Guard g = locks.acquire("same", LockMode::Read);
        readerIn = true;
    });
    std::this_thread::sleep_for(100ms);
    CHECK(!readerIn.load());
    guard.release();
    reader.join();
    CHECK(readerIn.load());
    CHECK(locks.tableSize() == 0);
}

// Locks are per file: holding "a" must not block "b".
void testFilesAreIndependent() {
    LockManager locks;
    LockManager::Guard holdA = locks.acquire("a", LockMode::Write);
    std::atomic<bool> gotB{false};
    std::thread other([&] {
        LockManager::Guard g = locks.acquire("b", LockMode::Write);
        gotB = true;
    });
    other.join();
    CHECK(gotB.load());
}

// Moving a guard transfers ownership; only the final owner releases.
void testGuardMove() {
    LockManager locks;
    LockManager::Guard outer;
    {
        LockManager::Guard inner = locks.acquire("f", LockMode::Write);
        outer = std::move(inner);
    }
    CHECK(locks.tableSize() == 1);  // still held by `outer`
    outer.release();
    CHECK(locks.tableSize() == 0);
}

}  // namespace

int main() {
    check::watchdog(120);
    testOppositeOrderDoesNotDeadlock();
    testMixedModesDoNotDeadlock();
    testDuplicateNamesAreMerged();
    testFilesAreIndependent();
    testGuardMove();
    return check::summary("test_lock_manager");
}
