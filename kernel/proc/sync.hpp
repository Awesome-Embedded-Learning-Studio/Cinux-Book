#pragma once

#include <stdint.h>

namespace cinux::proc {

class Spinlock {
public:
    Spinlock() = default;

    void acquire() {
        while (__atomic_test_and_set(&locked_, __ATOMIC_ACQUIRE))
            __asm__ volatile("pause");
    }

    void release() {
        __atomic_clear(&locked_, __ATOMIC_RELEASE);
    }

    [[nodiscard]] auto guard() {
        return Guard(this);
    }

private:
    volatile bool locked_ = false;

    class Guard {
    public:
        explicit Guard(Spinlock* lock) : lock_(lock) {
            lock_->acquire();
        }

        ~Guard() {
            lock_->release();
        }

        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

    private:
        Spinlock* lock_;
    };
};

}  // namespace cinux::proc
