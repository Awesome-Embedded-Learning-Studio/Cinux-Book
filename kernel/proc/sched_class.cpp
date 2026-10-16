#include "kernel/proc/sched_class.hpp"

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

    Task* take_front() { return count_ == 0 ? nullptr : take_at(0); }

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
        unsigned long long best = 0;
        for (unsigned long long i = 1; i < count_; ++i) {
            if (slots_[i]->priority < slots_[best]->priority) {
                best = i;
            }
        }
        return count_ == 0 ? nullptr : take_at(best);
    }

private:
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
