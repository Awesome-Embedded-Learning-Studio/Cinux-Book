#include "kernel/proc/sched_class.hpp"

#include <utility>

#include "kernel/arch/x86_64/per_cpu.hpp"
#include "kernel/proc/proc_config.hpp"
#include "kernel/proc/task.hpp"

namespace cinux::proc {

namespace {

class ClassQueue {
public:
    bool push(Task& task) {
        if (count_ == kReadyQueueCapacity) {
            return false;
        }
        slots_[count_] = &task;
        count_++;
        return true;
    }

    Task* take_at(unsigned long long slot) {
        Task* const kChosen = slots_[slot];
        for (unsigned long long i = slot; i + 1 < count_; ++i) {
            slots_[i] = slots_[i + 1];
        }
        count_--;
        return kChosen;
    }

    Task* take_front() {
        for (unsigned long long slot = 0; slot < count_; ++slot) {
            if (available(*slots_[slot])) {
                return take_at(slot);
            }
        }
        return nullptr;
    }

    bool remove(Task& task) {
        for (unsigned long long i = 0; i < count_; ++i) {
            if (slots_[i] == &task) {
                take_at(i);
                return true;
            }
        }
        return false;
    }

    Task* take_best_priority() {
        unsigned long long best = count_;
        for (unsigned long long i = 0; i < count_; ++i) {
            if (available(*slots_[i]) &&
                (best == count_ || slots_[i]->priority < slots_[best]->priority)) {
                best = i;
            }
        }
        return best == count_ ? nullptr : take_at(best);
    }

private:
    static bool available(const Task& task) {
        const int kOwner = __atomic_load_n(&task.on_cpu, __ATOMIC_ACQUIRE);
        return kOwner == -1 || std::cmp_equal(kOwner, arch::per_cpu::PerCpu::self().cpu_id);
    }
    Task*              slots_[kReadyQueueCapacity] = {};
    unsigned long long count_                      = 0;
};

class FifoClass final : public SchedulingClass {
public:
    [[nodiscard]] const char* name() const override { return "fifo"; }

    bool enqueue(Task& task) override { return row_.push(task); }

    [[nodiscard]] Task* pick() override { return row_.take_front(); }

    bool remove(Task& task) override { return row_.remove(task); }

    [[nodiscard]] bool wants_rotate(unsigned int slice_ticks) const override {
        return slice_ticks >= kTimeSliceTicks;
    }

private:
    ClassQueue row_{};
};

class PriorityClassImpl final : public SchedulingClass {
public:
    [[nodiscard]] const char* name() const override { return "priority"; }

    bool enqueue(Task& task) override { return row_.push(task); }

    [[nodiscard]] Task* pick() override { return row_.take_best_priority(); }

    bool remove(Task& task) override { return row_.remove(task); }

    [[nodiscard]] bool wants_rotate(unsigned int slice_ticks) const override {
        return slice_ticks >= kTimeSliceTicks;
    }

private:
    ClassQueue row_{};
};

}  // namespace

SchedulingClass& DefaultFifoClass() {
    static constinit FifoClass instance{};
    return instance;
}

SchedulingClass& PriorityClass() {
    static constinit PriorityClassImpl instance{};
    return instance;
}

}  // namespace cinux::proc
