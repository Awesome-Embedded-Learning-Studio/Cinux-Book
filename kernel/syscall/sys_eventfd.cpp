#include "kernel/syscall/sys_eventfd.hpp"

#include <atomic>
#include <cstdint>

#include "cinux/memory.hpp"
#include "cinux/ref_count.hpp"
#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/syscall/sys_ppoll.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {
namespace {
constexpr unsigned long long kEventfdSemaphore = 1;
constexpr uint64_t           kCounterMax       = ~uint64_t{0} - 1;
struct Counter : base::RefCount<> {
    std::atomic<uint64_t> value{0};
    std::atomic<unsigned long long> write_sequence{0};
    bool                  semaphore = false;
};
// NOLINTNEXTLINE(readability-function-size) FsOps callback signature.
base::Result<unsigned long> read_counter(void* context, [[maybe_unused]] fs::FsNode node,
                                         [[maybe_unused]] unsigned long offset, void* sink,
                                         unsigned long count) {
    if (count < sizeof(uint64_t)) {
        return base::KernelError::kInvalidArgument;
    }
    auto* counter = static_cast<Counter*>(context);
    auto  value   = counter->value.load();
    for (;;) {
        if (value == 0) {
            return base::KernelError::kWouldBlock;
        }
        const auto kTaken = counter->semaphore ? uint64_t{1} : value;
        if (counter->value.compare_exchange_weak(value, value - kTaken)) {
            base::CopyBytes(sink, &kTaken, sizeof(kTaken));
            return sizeof(kTaken);
        }
    }
}
// NOLINTNEXTLINE(readability-function-size) FsOps callback signature.
base::Result<unsigned long> write_counter(void* context, [[maybe_unused]] fs::FsNode node,
                                          [[maybe_unused]] unsigned long offset, const void* source,
                                          unsigned long count) {
    if (count < sizeof(uint64_t)) {
        return base::KernelError::kInvalidArgument;
    }
    uint64_t added = 0;
    base::CopyBytes(&added, source, sizeof(added));
    if (added > kCounterMax) {
        return base::KernelError::kInvalidArgument;
    }
    auto* counter = static_cast<Counter*>(context);
    auto  value   = counter->value.load();
    for (;;) {
        if (added > kCounterMax - value) {
            return base::KernelError::kWouldBlock;
        }
        if (counter->value.compare_exchange_weak(value, value + added)) {
            counter->write_sequence.fetch_add(1);
            return sizeof(added);
        }
    }
}
void retain_counter(void* context, [[maybe_unused]] fs::FsNode node) {
    static_cast<Counter*>(context)->retain();
}
void release_counter(void* context, [[maybe_unused]] fs::FsNode node) {
    auto* counter = static_cast<Counter*>(context);
    if (counter->release()) {
        delete counter;
    }
}
bool readable_counter(void* context, [[maybe_unused]] fs::FsNode node) {
    return static_cast<Counter*>(context)->value.load() != 0;
}
const fs::FsOps kCounterOps{
    .read     = read_counter,
    .write    = write_counter,
    .retain   = retain_counter,
    .release  = release_counter,
    .readable = readable_counter,
};
}
long long HandleEventfd2(const SyscallFrame& frame) {
    if ((frame.rsi & ~(kOpenCloexec | kOpenNonblock | kEventfdSemaphore)) != 0) {
        return -kEinval;
    }
    const base::Shared<Counter> kCounter(new Counter);
    kCounter->value.store(static_cast<unsigned int>(frame.rdi));
    kCounter->semaphore = (frame.rsi & kEventfdSemaphore) != 0;
    const auto kDescriptor =
        CurrentTask()->files->alloc(&kCounterOps, kCounter.get(), {},
                                    {
                                        .status  = kOpenReadWrite | (frame.rsi & kOpenNonblock),
                                        .cloexec = (frame.rsi & kOpenCloexec) != 0,
                                        .limit   = FileDescriptorLimit(),
                                    });
    return kDescriptor.ok() ? static_cast<long long>(kDescriptor.value()) : -kEmfile;
}
bool PollEventCounter(const fs::OpenFile* file, unsigned int events, unsigned short& ready,
                       unsigned long long* write_sequence) {
    if (file->ops != &kCounterOps) {
        return false;
    }
    const auto* counter = static_cast<Counter*>(file->backend);
    // Read the notification sequence before readiness, so a racing writer
    // remains observable on the next scan even if this scan sees its value.
    if (write_sequence != nullptr) {
        *write_sequence = counter->write_sequence.load();
    }
    const auto kValue = counter->value.load();
    ready = static_cast<unsigned short>((kValue != 0 ? events & kPollIn : 0U) |
                                        (kValue < kCounterMax ? events & kPollOut : 0U));
    return true;
}
}
