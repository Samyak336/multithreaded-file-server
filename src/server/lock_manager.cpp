#include "server/lock_manager.hpp"

#include <algorithm>

namespace mtfs {

LockManager::Guard::Guard(Guard&& other) noexcept : owner_(other.owner_), held_(std::move(other.held_)) {
    other.owner_ = nullptr;
    other.held_.clear();
}

LockManager::Guard& LockManager::Guard::operator=(Guard&& other) noexcept {
    if (this != &other) {
        release();
        owner_ = other.owner_;
        held_ = std::move(other.held_);
        other.owner_ = nullptr;
        other.held_.clear();
    }
    return *this;
}

void LockManager::Guard::release() {
    // Unlocking never blocks for long, so the order does not matter for
    // deadlock freedom; reverse order is simply the conventional choice.
    for (auto it = held_.rbegin(); it != held_.rend(); ++it) owner_->unlock(*it);
    held_.clear();
}

LockManager::Guard LockManager::acquire(const std::string& name, LockMode mode) {
    return acquireAll({{name, mode}});
}

LockManager::Guard LockManager::acquireAll(std::vector<std::pair<std::string, LockMode>> requests) {
    // The global lock order: ascending by name.
    std::sort(requests.begin(), requests.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // Merge duplicates. Taking the same non-recursive lock twice would make
    // a thread wait for itself forever.
    std::vector<std::pair<std::string, LockMode>> plan;
    for (auto& request : requests) {
        if (!plan.empty() && plan.back().first == request.first) {
            if (request.second == LockMode::Write) plan.back().second = LockMode::Write;
        } else {
            plan.push_back(std::move(request));
        }
    }

    Guard guard;
    guard.owner_ = this;
    guard.held_.reserve(plan.size());
    for (const auto& [name, mode] : plan) {
        Entry* entry = pin(name);  // the pin keeps the entry alive while we block below
        if (mode == LockMode::Read) {
            entry->lock.lockRead();
        } else {
            entry->lock.lockWrite();
        }
        guard.held_.push_back({name, mode, entry});
    }
    return guard;
}

LockManager::Entry* LockManager::pin(const std::string& name) {
    MutexGuard g(tableMutex_);
    std::unique_ptr<Entry>& slot = table_[name];
    if (!slot) slot = std::make_unique<Entry>();
    ++slot->users;
    return slot.get();
}

void LockManager::unpin(const std::string& name) {
    MutexGuard g(tableMutex_);
    auto it = table_.find(name);
    if (it != table_.end() && --it->second->users == 0) table_.erase(it);
}

void LockManager::unlock(const Guard::Held& held) {
    if (held.mode == LockMode::Read) {
        held.entry->lock.unlockRead();
    } else {
        held.entry->lock.unlockWrite();
    }
    unpin(held.name);
}

std::size_t LockManager::tableSize() const {
    MutexGuard g(tableMutex_);
    return table_.size();
}

}  // namespace mtfs
