#include "kernel/proc/lockdep.hpp"

#include <algorithm>

#include "cinux/assert.hpp"

namespace cinux::proc {

namespace {

LockdepSink g_lockdep_sink = nullptr;

}  // namespace

void InstallLockdepSink(LockdepSink sink) {
    g_lockdep_sink = sink;
}

unsigned int LockOrder::find_lock(const void* lock) const {
    return static_cast<unsigned int>(
        locks_.find_if([lock](const void* resident) { return resident == lock; }));
}

unsigned int LockOrder::vacant_slot() const {
    return static_cast<unsigned int>(locks_.vacant());
}

LockOrder::Status LockOrder::acquire(const void* lock) {
    if (lock == nullptr || held_.full()) {
        return Status::kCapacity;
    }
    unsigned int index = find_lock(lock);
    if (index == kLocks) {
        index = vacant_slot();
        if (index == kLocks) {
            return Status::kCapacity;
        }
    }
    for (const auto kHeld : held_.view()) {
        if (kHeld == index) {
            return Status::kRecursive;
        }
        if (graph_.reaches(index, kHeld)) {
            return Status::kCycle;
        }
    }
    if (locks_.get(index) == nullptr) {
        base::safety::Check(locks_.insert(lock) == index, "lockdep identity slot changed");
    }
    for (const auto kHeld : held_.view()) {
        graph_.connect(kHeld, index);
    }
    base::safety::Check(held_.push(index), "lockdep held stack lost capacity");
    return Status::kOk;
}

LockOrder::Status LockOrder::release(const void* lock) {
    const unsigned int kIndex = find_lock(lock);
    const auto         kHeld  = held_.view();
    if (kIndex == kLocks || std::ranges::find(kHeld, kIndex) == kHeld.end()) {
        return Status::kNotHeld;
    }
    if (*held_.top() != kIndex) {
        return Status::kOutOfOrder;
    }
    base::safety::Check(held_.pop(), "lockdep held stack lost its top");
    return Status::kOk;
}

bool LockOrder::forget(const void* lock) {
    const unsigned int kIndex = find_lock(lock);
    if (kIndex == kLocks) {
        return true;
    }
    const auto kHeld = held_.view();
    if (std::ranges::find(kHeld, kIndex) != kHeld.end()) {
        return false;
    }
    base::safety::Check(locks_.erase(kIndex), "lockdep identity slot vanished");
    names_[kIndex] = nullptr;
    graph_.erase(kIndex);
    return true;
}

void LockOrder::name_lock(const void* lock, const char* label) {
    if (lock == nullptr || label == nullptr) {
        return;
    }
    unsigned int index = find_lock(lock);
    if (index == kLocks) {
        index = vacant_slot();
        base::safety::Check(index != kLocks, "lockdep name registry lost capacity");
        base::safety::Check(locks_.insert(lock) == index, "lockdep identity slot changed");
    }
    names_[index] = label;
}

void LockOrder::dump_held() {
    if (g_lockdep_sink == nullptr) {
        return;
    }
    for (const auto kHeld : held_.view()) {
        const void* const kResident = *locks_.get(kHeld);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        g_lockdep_sink(names_[kHeld], reinterpret_cast<unsigned long long>(kResident));
    }
}

}  // namespace cinux::proc
