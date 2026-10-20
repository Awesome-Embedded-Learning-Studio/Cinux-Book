#include "kernel/syscall/sys_ioctl.hpp"

#include "cinux/result.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/driver/tty/tty_config.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

namespace {

/// One pointer-carrying command's span: how many bytes, and whether
/// the caller hands them over or takes them back. Zero bytes marks the
/// wordless commands, whose argument is a flag, not an address.
struct WordSpan {
    unsigned long long word;
    unsigned long      bytes;
    bool               caller_gives;
};

const WordSpan kWordSpans[] = {
    {.word = driver::kTtyTcgets, .bytes = driver::kTermiosBytes, .caller_gives = false},
    {.word = driver::kTtyTcsets, .bytes = driver::kTermiosBytes, .caller_gives = true},
    {.word = driver::kTtyTcsetsw, .bytes = driver::kTermiosBytes, .caller_gives = true},
    {.word = driver::kTtyTcsetsf, .bytes = driver::kTermiosBytes, .caller_gives = true},
    {.word = driver::kTtyTiocsctty, .bytes = 0, .caller_gives = true},
    {.word = driver::kTtyTiocgpgrp, .bytes = 4, .caller_gives = false},
    {.word = driver::kTtyTiocspgrp, .bytes = 4, .caller_gives = true},
    {.word = driver::kTtyTiocgwinsz, .bytes = 8, .caller_gives = false},
    {.word = driver::kTtyTiocsptlck, .bytes = 4, .caller_gives = true},
    {.word = driver::kTtyTiocgptn, .bytes = 4, .caller_gives = false},
};

/// The span one command's argument carries, or nullptr when unknown.
const WordSpan* span_of(unsigned long long word) {
    for (const WordSpan& span : kWordSpans) {
        if (span.word == word) {
            return &span;
        }
    }
    return nullptr;
}

}  // namespace

long long HandleIoctl(const SyscallFrame& frame) {
    const arch::IrqGuard              kGuard;
    const base::Result<fs::OpenFile*> kSlot = LiveSlot(frame.rdi);
    if (!kSlot.ok()) {
        return -kEbadf;
    }
    const fs::OpenFile* const kFile = kSlot.value();
    if (kFile->ops->ioctl == nullptr) {
        return -kEnotty;
    }
    const WordSpan* const kSpan = span_of(frame.rsi);
    if (kSpan == nullptr) {
        return -kEnotty;
    }
    if (kSpan->bytes == 0) {
        return kFile->ops->ioctl(kFile->backend, kFile->node, frame.rsi, frame.rdx);
    }
    if (frame.rdx == 0 || frame.rdx >= kUserAddressLimit) {
        return -kEfault;
    }
    const bool kReachable = kSpan->caller_gives ? UserRangeReadable(frame.rdx, kSpan->bytes)
                                                : UserRangeWritable(frame.rdx, kSpan->bytes);
    if (!kReachable) {
        return -kEfault;
    }
    return kFile->ops->ioctl(kFile->backend, kFile->node, frame.rsi, frame.rdx);
}

}  // namespace cinux::syscall
