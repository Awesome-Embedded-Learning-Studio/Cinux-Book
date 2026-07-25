/**
 * @file kernel/drivers/input/input_event_device.cpp
 * @brief /dev/event0 character device: userspace input event read/poll (F-GUI-USERSPACE b2)
 *
 * Kernel-only (linked into big_kernel_common, NOT host unit tests) -- it pulls
 * in the scheduler wait primitives, so it is kept out of the InodeOps header
 * and the GUI event header stays host-clean.  Mirrors the fb_dev.cpp /
 * pipe.cpp / pty_device.cpp split.
 *
 * Namespace: cinux::input
 */

#include "kernel/drivers/input/input_event_device.hpp"

#include <stdint.h>

#include <cstring>  // std::memcpy (sys_read hands a kernel staging buffer, not the user pointer)

#include "kernel/gui/event.hpp"       // cinux::gui::Event
#include "kernel/net/wait_queue.hpp"  // cinux::net::wait_enqueue/wake_all/wait_remove
#include "kernel/proc/process.hpp"    // cinux::proc::Task (complete type)
#include "kernel/proc/scheduler.hpp"  // prepare_to_wait/schedule_blocked/current

namespace cinux::input {

// ============================================================
// InputEventDevice singleton
// ============================================================

InputEventDevice& InputEventDevice::instance() {
    static InputEventDevice dev;
    return dev;
}

void InputEventDevice::push_event(const cinux::gui::Event& ev) {
    // irq_guard: serialises the two producers (mouse IRQ12 + keyboard listener)
    // against the reader, and closes the lost-wakeup window vs a reader in the
    // middle of prepare_to_wait().  wake_all() only flips waiters to runnable
    // (Scheduler::unblock) -- it must not schedule() inline from the ISR
    // (sti-in-syscall -> LAPIC tick traps the kernel stack -> sysret corrupts).
    auto g = lock_.irq_guard();
    if (!events_.full()) {
        events_.push(ev);  // silent drop on overflow (matches the GUI EventQueue)
    }
    cinux::net::wake_all(read_waiters_);
}

// ============================================================
// InputEventDeviceOps -- /dev/event0 read / poll
// ============================================================

cinux::lib::ErrorOr<int64_t> InputEventDeviceOps::read(const cinux::fs::Inode*, uint64_t, void* buf,
                                                       uint64_t count) {
    // evdev semantics: a read smaller than one event is EINVAL (mirrors Linux
    // /dev/input/eventN, which requires at least struct input_event bytes).
    if (count < sizeof(cinux::gui::Event)) {
        return cinux::lib::Error::InvalidArgument;
    }

    auto& dev = InputEventDevice::instance();
    for (;;) {
        bool need_block = false;
        {
            // prepare_to_wait() under the same irq_guard that checks the queue
            // closes the lost-wakeup window vs a concurrent push_event() (the
            // PTY/pipe pattern).  buf is a KERNEL staging buffer handed in by
            // sys_read -- write it directly with memcpy; sys_read performs the
            // copy_to_user to the real user pointer afterwards (so the blocking
            // read stays AC=0 safe, same as the console TTY / PTY read paths).
            auto              guard = dev.lock_.irq_guard();
            cinux::gui::Event ev{};
            if (dev.events_.pop(ev)) {
                std::memcpy(buf, &ev, sizeof(ev));
                return static_cast<int64_t>(sizeof(ev));
            }

            cinux::proc::Task* self = cinux::proc::Scheduler::current();
            if (self == nullptr) {
                return static_cast<int64_t>(0);  // early/test context with no scheduler
            }
            cinux::net::wait_enqueue(dev.read_waiters_, self);
            cinux::proc::Scheduler::prepare_to_wait(self);
            need_block = true;
        }  // IRQs restored + lock released before switching out.

        if (need_block) {
            cinux::proc::Scheduler::schedule_blocked();
        }
    }
}

uint32_t InputEventDeviceOps::poll_events(const cinux::fs::Inode*, cinux::proc::Task* waiter,
                                          bool* registered) {
    // Pipe poll pattern: report readiness AND register the waiter atomically
    // under the same lock that push_event() takes, so a push between the check
    // and the park is never missed.
    auto&    dev  = InputEventDevice::instance();
    auto     g    = dev.lock_.irq_guard();
    uint32_t mask = 0;
    if (!dev.events_.empty()) {
        mask |= cinux::fs::kPollIn;
    }
    if (waiter != nullptr) {
        cinux::net::wait_enqueue(dev.read_waiters_, waiter);
        if (registered != nullptr) {
            *registered = true;
        }
    }
    return mask;
}

void InputEventDeviceOps::poll_detach_waiter(const cinux::fs::Inode*, cinux::proc::Task* waiter) {
    auto& dev = InputEventDevice::instance();
    auto  g   = dev.lock_.irq_guard();
    cinux::net::wait_remove(dev.read_waiters_, waiter);
}

// ============================================================
// Ops factory
// ============================================================

namespace {
InputEventDeviceOps g_input_event_ops;
}  // namespace

cinux::fs::InodeOps& input_event_device_ops() {
    return g_input_event_ops;
}

}  // namespace cinux::input
