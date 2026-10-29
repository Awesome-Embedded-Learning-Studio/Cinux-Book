#include "kernel/syscall/sys_epoll.hpp"

#include <array>
#include <cstddef>

#include "cinux/assert.hpp"
#include "cinux/container/slot_table.hpp"
#include "cinux/ref_count.hpp"
#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signals.hpp"
#include "kernel/proc/sync.hpp"
#include "kernel/syscall/signal.hpp"
#include "kernel/syscall/sys_clock.hpp"
#include "kernel/syscall/sys_eventfd.hpp"
#include "kernel/syscall/sys_ppoll.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"
#include "kernel/syscall/uaccess.hpp"
#include "kernel/syscall/wait_time.hpp"

namespace cinux::syscall {
namespace {
constexpr unsigned long kEpollLimit    = 64;
constexpr unsigned long kWatchLimit    = 64;
constexpr unsigned long kCtlAdd        = 1;
constexpr unsigned long kCtlDelete     = 2;
constexpr unsigned long kCtlModify     = 3;
constexpr unsigned int  kEdgeTriggered = 0x80000000U;
constexpr unsigned int  kOneShot       = 0x40000000U;
struct [[gnu::packed]] EpollEvent {
    unsigned int       events = 0;
    unsigned long long data   = 0;
};
static_assert(sizeof(EpollEvent) == 12);
static_assert(offsetof(EpollEvent, data) == 4);
struct Watch {
    fs::OpenFile* file       = nullptr;
    unsigned long descriptor = 0;
    EpollEvent    event{};
    unsigned long long version        = 0;
    unsigned long long write_sequence = 0;
    bool               edge_pending   = true;
};
struct Epoll : base::RefCount<> {
    base::container::SlotTable<Watch, kWatchLimit> watches{};
    unsigned long                                  cursor = 0;
    unsigned long long                             next_version = 0;
    ~Epoll();
};
struct Snapshot {
    base::Shared<fs::OpenFile> file;
    EpollEvent                 event;
    unsigned long              index = 0;
    unsigned long long         version = 0;
};
class EpollRegistry {
public:
    static EpollRegistry& self() {
        static EpollRegistry registry;
        return registry;
    }
    bool seat(Epoll* instance) {
        const proc::SpinGuard kGuard(gate_);
        return instances_.insert(instance) != kEpollLimit;
    }
    void retire(Epoll* instance) {
        const proc::SpinGuard kGuard(gate_);
        const auto            kIndex =
            instances_.find_if([instance](const Epoll* entry) { return entry == instance; });
        if (kIndex != kEpollLimit) {
            base::safety::Check(instances_.erase(kIndex), "epoll registry lost instance");
        }
    }
    void forget(const fs::OpenFile* file) {
        const proc::SpinGuard kGuard(gate_);
        instances_.for_each([file](unsigned long, Epoll* instance) {
            instance->watches.for_each([file, instance](unsigned long index, const Watch& watch) {
                if (watch.file == file) {
                    base::safety::Check(instance->watches.erase(index), "epoll watch vanished");
                }
            });
        });
    }
    unsigned long snapshot(const Epoll& instance, std::array<Snapshot, kWatchLimit>& out) {
        const proc::SpinGuard kGuard(gate_);
        unsigned long         count = 0;
        for (unsigned long step = 0; step < kWatchLimit; ++step) {
            const auto  kIndex = (instance.cursor + step) % kWatchLimit;
            const auto* watch  = instance.watches.get(kIndex);
            if (watch != nullptr && watch->file->try_retain()) {
                out[count++] = {
                    .file  = base::Shared<fs::OpenFile>(watch->file),
                    .event = watch->event,
                    .index = kIndex,
                    .version = watch->version,
                };
            }
        }
        return count;
    }
    void advance(Epoll& instance, unsigned long index) {
        const proc::SpinGuard kGuard(gate_);
        instance.cursor = (index + 1) % kWatchLimit;
    }
    bool claim(Epoll& instance, const Snapshot& item, unsigned long long sequence) {
        const proc::SpinGuard kGuard(gate_);
        auto* watch = instance.watches.get(item.index);
        if (watch == nullptr || watch->version != item.version) {
            return false;
        }
        if ((watch->event.events & kEdgeTriggered) != 0) {
            if (!watch->edge_pending && watch->write_sequence == sequence) {
                return false;
            }
            watch->write_sequence = sequence;
            watch->edge_pending   = false;
        }
        return true;
    }
    void retry(Epoll& instance, const Snapshot& item) {
        const proc::SpinGuard kGuard(gate_);
        auto* watch = instance.watches.get(item.index);
        if (watch != nullptr && watch->version == item.version) {
            watch->edge_pending = true;
        }
    }
    long long control_add(Epoll& instance, fs::OpenFile* file, unsigned long descriptor,
                          EpollEvent event);
    long long control_delete(Epoll& instance, fs::OpenFile* file, unsigned long descriptor);
    long long control_modify(Epoll& instance, fs::OpenFile* file, unsigned long descriptor,
                             EpollEvent event);

private:
    proc::Spinlock                                  gate_{};
    base::container::SlotTable<Epoll*, kEpollLimit> instances_{};
};
Epoll::~Epoll() {
    EpollRegistry::self().retire(this);
}
void drop_watches(const fs::OpenFile* file) {
    EpollRegistry::self().forget(file);
}
long long EpollRegistry::control_add(Epoll& instance, fs::OpenFile* file, unsigned long descriptor,
                                     EpollEvent event) {
    const proc::SpinGuard kGuard(gate_);
    const auto            kFound = instance.watches.find_if(
        [&](const Watch& watch) { return watch.file == file && watch.descriptor == descriptor; });
    if (kFound != kWatchLimit) {
        return -kEexist;
    }
    const Watch kWatch{.file = file, .descriptor = descriptor, .event = event,
                        .version = ++instance.next_version};
    if (instance.watches.insert(kWatch) == kWatchLimit) {
        return -kEnomem;
    }
    file->on_last_close.store(drop_watches);
    return 0;
}
long long EpollRegistry::control_delete(Epoll& instance, fs::OpenFile* file,
                                        unsigned long descriptor) {
    const proc::SpinGuard kGuard(gate_);
    const auto            kIndex = instance.watches.find_if(
        [&](const Watch& watch) { return watch.file == file && watch.descriptor == descriptor; });
    if (kIndex == kWatchLimit) {
        return -kEnoent;
    }
    base::safety::Check(instance.watches.erase(kIndex), "epoll watch vanished");
    return 0;
}
long long EpollRegistry::control_modify(Epoll& instance, fs::OpenFile* file,
                                        unsigned long descriptor, EpollEvent event) {
    const proc::SpinGuard kGuard(gate_);
    const auto            kIndex = instance.watches.find_if(
        [&](const Watch& watch) { return watch.file == file && watch.descriptor == descriptor; });
    if (kIndex == kWatchLimit) {
        return -kEnoent;
    }
    auto* watch         = instance.watches.get(kIndex);
    watch->event        = event;
    watch->version      = ++instance.next_version;
    watch->edge_pending = true;
    return 0;
}
void retain_epoll(void* context, [[maybe_unused]] fs::FsNode node) {
    static_cast<Epoll*>(context)->retain();
}
void release_epoll(void* context, [[maybe_unused]] fs::FsNode node) {
    auto* instance = static_cast<Epoll*>(context);
    if (instance->release()) {
        delete instance;
    }
}
// NOLINTBEGIN(misc-const-correctness) FsOps callback signatures require mutable erased buffers.
// NOLINTNEXTLINE(readability-function-size) FsOps callback signature.
base::Result<unsigned long> refuse_read([[maybe_unused]] void*         context,
                                        [[maybe_unused]] fs::FsNode    node,
                                        [[maybe_unused]] unsigned long offset,
                                        [[maybe_unused]] void*         sink,
                                        [[maybe_unused]] unsigned long count) {
    return base::KernelError::kInvalidArgument;
}
// NOLINTNEXTLINE(readability-function-size,misc-const-correctness) FsOps callback signature.
base::Result<unsigned long> refuse_write([[maybe_unused]] void*         context,
                                         [[maybe_unused]] fs::FsNode    node,
                                         [[maybe_unused]] unsigned long offset,
                                         [[maybe_unused]] const void*   source,
                                         [[maybe_unused]] unsigned long count) {
    return base::KernelError::kInvalidArgument;
}
// NOLINTNEXTLINE(misc-const-correctness) FsOps callback signature.
bool no_input([[maybe_unused]] void* context, [[maybe_unused]] fs::FsNode node) {
    return false;
}
// NOLINTEND(misc-const-correctness)
const fs::FsOps kEpollOps{
    .read     = refuse_read,
    .write    = refuse_write,
    .retain   = retain_epoll,
    .release  = release_epoll,
    .readable = no_input,
};
Epoll* instance_of(const base::Shared<fs::OpenFile>& file) {
    return file->ops == &kEpollOps ? static_cast<Epoll*>(file->backend) : nullptr;
}
long long scan_events(Epoll& instance, Word address, unsigned long capacity) {
    std::array<Snapshot, kWatchLimit> snapshot{};
    const auto                        kSize = EpollRegistry::self().snapshot(instance, snapshot);
    unsigned long                     count = 0;
    for (unsigned long index = 0; index < kSize; ++index) {
        const auto& item = snapshot[index];
        unsigned short ready = 0;
        unsigned long long sequence = 0;
        if ((item.event.events & kEdgeTriggered) != 0) {
            PollEventCounter(item.file.get(), item.event.events, ready, &sequence);
        } else {
            ready = PollOpen(item.file.get(), item.event.events | kPollErr | kPollHup);
        }
        if (ready == 0 || !EpollRegistry::self().claim(instance, item, sequence)) {
            continue;
        }
        const EpollEvent kEvent{.events = ready, .data = item.event.data};
        if (!uaccess::CopyToUser(address + (count * sizeof(EpollEvent)), &kEvent, sizeof(kEvent))) {
            EpollRegistry::self().retry(instance, item);
            return -kEfault;
        }
        EpollRegistry::self().advance(instance, item.index);
        if (++count == capacity) {
            break;
        }
    }
    return static_cast<long long>(count);
}
}
long long HandleEpollCreate1(const SyscallFrame& frame) {
    if ((frame.rdi & ~kOpenCloexec) != 0) {
        return -kEinval;
    }
    const base::Shared<Epoll> kInstance(new Epoll);
    if (!EpollRegistry::self().seat(kInstance.get())) {
        return -kEmfile;
    }
    const auto kDescriptor = CurrentTask()->files->alloc(
        &kEpollOps, kInstance.get(), {},
        {.status = kOpenReadWrite, .cloexec = frame.rdi != 0, .limit = FileDescriptorLimit()});
    return kDescriptor.ok() ? static_cast<long long>(kDescriptor.value()) : -kEmfile;
}
long long HandleEpollCtl(const SyscallFrame& frame) {
    const auto kEpoll = LiveSlot(frame.rdi);
    const auto kFile  = LiveSlot(frame.rdx);
    if (!kEpoll.ok() || !kFile.ok()) {
        return -kEbadf;
    }
    Epoll* instance = instance_of(kEpoll.value());
    if (instance == nullptr || frame.rdi == frame.rdx) {
        return -kEinval;
    }
    if (kFile.value()->ops == &kEpollOps) {
        return -kEopnotsupp;
    }
    if (frame.rsi < kCtlAdd || frame.rsi > kCtlModify) {
        return -kEinval;
    }
    EpollEvent event{};
    if (frame.rsi != kCtlDelete && !uaccess::CopyFromUser(&event, frame.r10, sizeof(event))) {
        return -kEfault;
    }
    if ((event.events & kOneShot) != 0) {
        return -kEopnotsupp;
    }
    if ((event.events & kEdgeTriggered) != 0) {
        // libuv uses edge-triggered EPOLLIN on eventfd without draining it.
        // Other backends and writable edges need their own wakeup tracking.
        unsigned short ready = 0;
        if ((event.events & ~(kEdgeTriggered | kPollIn | kPollErr | kPollHup)) != 0 ||
            !PollEventCounter(kFile.value().get(), event.events, ready)) {
            return -kEopnotsupp;
        }
    }
    auto& registry = EpollRegistry::self();
    if (frame.rsi == kCtlAdd) {
        return registry.control_add(*instance, kFile.value().get(), frame.rdx, event);
    }
    if (frame.rsi == kCtlDelete) {
        return registry.control_delete(*instance, kFile.value().get(), frame.rdx);
    }
    return registry.control_modify(*instance, kFile.value().get(), frame.rdx, event);
}
long long HandleEpollWait(const SyscallFrame& frame) {
    const auto kEpoll = LiveSlot(frame.rdi);
    if (!kEpoll.ok()) {
        return -kEbadf;
    }
    Epoll*     instance  = instance_of(kEpoll.value());
    const auto kCapacity = static_cast<int>(frame.rdx);
    if (instance == nullptr || kCapacity <= 0 || kCapacity > 1024) {
        return -kEinval;
    }
    if (!UserRangeWritable(frame.rsi, static_cast<unsigned long>(kCapacity) * sizeof(EpollEvent))) {
        return -kEfault;
    }
    const auto kDeadline = MillisecondDeadline(static_cast<int>(frame.r10));
    for (;;) {
        const auto kReady =
            scan_events(*instance, frame.rsi, static_cast<unsigned long>(kCapacity));
        if (kReady != 0) {
            return kReady;
        }
        if (proc::HasPendingSignal(*CurrentTask())) {
            return -kEintr;
        }
        if (MonotonicNanoseconds() >= kDeadline) {
            return 0;
        }
        proc::Scheduler::self().yield();
    }
}
long long HandleEpollPwait(const SyscallFrame& frame) {
    auto*      self     = CurrentTask();
    const auto kRestore = self->signals.blocked;
    if (frame.r8 != 0) {
        if (frame.r9 != sizeof(SigSet)) {
            return -kEinval;
        }
        SigSet wanted{};
        if (!uaccess::CopyFromUser(&wanted, frame.r8, sizeof(wanted))) {
            return -kEfault;
        }
        self->signals.blocked = MaskableSignals(wanted);
    }
    const auto kResult = HandleEpollWait(frame);
    if (frame.r8 != 0) {
        proc::FinishSignalWait(*self, kRestore);
    }
    return kResult;
}
}
