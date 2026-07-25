/**
 * @file kernel/drivers/input/input_event_device.hpp
 * @brief /dev/event0 InodeOps: userspace input event device (F-GUI-USERSPACE batch 2)
 *
 * Bridges decoded kernel input events (mouse + keyboard) to userspace via a
 * character device node, mirroring the role of Linux's evdev (/dev/input/event*).
 * Producers are hard-IRQ contexts: the PS/2 + USB mouse ISRs and the keyboard
 * listener installed by gui_init (see gui_init.cpp::on_key_event).  They call
 * InputEventDevice::push_event(); userspace drains the queue through
 * read()/poll() on /dev/event0.
 *
 * MPSC safety: there are two producers (mouse IRQ12 + keyboard via the GUI
 * listener), so the lock-free SPSC EventQueue used by the GUI cannot be shared
 * here (it assumes a single consumer as well).  This device therefore keeps
 * its own RingBuffer guarded by a Spinlock -- the irq_guard also makes the
 * prepare_to_wait() in read() atomic vs a concurrent push_event(), closing the
 * lost-wakeup window the way PTY/pipe reads do.
 *
 * Namespace: cinux::input
 */

#pragma once

#include <stdint.h>

#include <cinux/expected.hpp>
#include <cinux/ring_buffer.hpp>

#include "kernel/fs/inode.hpp"   // InodeOps, kPoll*
#include "kernel/gui/event.hpp"  // cinux::gui::Event (the queue element)
#include "kernel/proc/sync.hpp"  // cinux::proc::Spinlock

namespace cinux::proc {
struct Task;  // forward -- read_waiters_ holds blocked reader tasks
}

namespace cinux::input {

class InputEventDeviceOps;  // forward (friend)

/// Capacity of the input-event ring buffer (matches the GUI EventQueue depth).
constexpr uint32_t kInputEventQueueSize = 128;

/**
 * @brief Singleton buffer of decoded input Events for /dev/event0.
 *
 * One system input device.  Producers call push_event() from IRQ context;
 * InputEventDeviceOps::read()/poll_events() drain it from syscall context.
 *
 * Members are private; InputEventDeviceOps is a friend so the ops adapter can
 * reach the queue + wait list (the same split the PTY / pipe ops use).
 */
class InputEventDevice {
public:
    /// The single /dev/event0 instance.
    static InputEventDevice& instance();

    /// Enqueue one event from a hard-IRQ handler (mouse ISR / keyboard listener).
    /// Drops silently if the buffer is full (matches the GUI EventQueue policy).
    /// Wakes every blocked reader.  ISR-safe: takes the Spinlock under
    /// irq_guard and only wake_all()s (Scheduler::unblock -- sets runnable,
    /// never schedule()s inline, per the sti-in-syscall #DF rule).
    void push_event(const cinux::gui::Event& ev);

private:
    InputEventDevice() = default;
    friend class InputEventDeviceOps;

    cinux::lib::RingBuffer<cinux::gui::Event, kInputEventQueueSize> events_;
    cinux::proc::Spinlock                                           lock_;
    cinux::proc::Task*                                              read_waiters_{nullptr};
};

/**
 * @brief /dev/event0 InodeOps adapter over InputEventDevice.
 *
 * read() blocks on the wait queue until an event arrives (prepare_to_wait +
 * schedule_blocked, the PTY pattern); poll() reports kPollIn when one is
 * buffered and registers the poller so push_event() wakes it.
 */
class InputEventDeviceOps : public cinux::fs::InodeOps {
public:
    cinux::lib::ErrorOr<int64_t> read(const cinux::fs::Inode* inode, uint64_t offset, void* buf,
                                      uint64_t count) override;
    uint32_t poll_events(const cinux::fs::Inode* inode, cinux::proc::Task* waiter,
                         bool* registered) override;
    void     poll_detach_waiter(const cinux::fs::Inode* inode, cinux::proc::Task* waiter) override;
};

/// Global /dev/event0 ops instance, registered by devfs::init().
cinux::fs::InodeOps& input_event_device_ops();

}  // namespace cinux::input
