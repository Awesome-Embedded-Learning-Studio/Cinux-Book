#include "kernel/syscall/sys_sendfile.hpp"

#include <cstdint>
#include <limits>
#include <span>

#include "cinux/bit_ops/bitmask.hpp"
#include "cinux/math.hpp"
#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/fs/pipe.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signals.hpp"
#include "kernel/syscall/signal.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"
#include "kernel/syscall/uaccess.hpp"

namespace cinux::syscall {
namespace {

bool regular_mode(std::uint32_t mode) {
    const base::bit::BitMask<std::uint32_t> kMode{mode};
    return kMode.extract({.low = 12, .width = 4}) == 8;
}

base::Result<bool> regular_file(const fs::OpenFile& file) {
    if (file.ops->stat == nullptr) {
        return false;
    }
    fs::FsStat stat{};
    const auto kStat = file.ops->stat(file.backend, file.node, stat);
    if (!kStat.ok()) {
        return kStat.error();
    }
    return regular_mode(stat.mode);
}

base::Result<unsigned long> read_chunk(const fs::OpenFile& file, unsigned long position,
                                       std::span<unsigned char> bytes) {
    return file.ops->read(file.backend, file.node, position, bytes.data(), bytes.size());
}

base::Result<unsigned long> write_chunk(const fs::OpenFile&            file,
                                        std::span<const unsigned char> bytes) {
    for (;;) {
        const auto kPut =
            file.ops == fs::PipeOps()
                ? static_cast<fs::Pipe*>(file.backend)->try_push(bytes.data(), bytes.size())
                : file.ops->write(file.backend, file.node, file.offset, bytes.data(), bytes.size());
        if (kPut.ok()) {
            return kPut.value();
        }
        if (kPut.error() != base::KernelError::kWouldBlock ||
            (file.status_flags & kOpenNonblock) != 0) {
            return kPut.error();
        }
        if (proc::HasPendingSignal(*CurrentTask())) {
            return base::KernelError::kInterrupted;
        }
        proc::Scheduler::self().yield();
    }
}

struct Transfer {
    fs::OpenFile& input;
    fs::OpenFile& output;
    unsigned long position;
    unsigned long remaining;
    bool          shared_input;
    bool          seekable_output;
};

base::Result<unsigned long> transfer_chunk(Transfer& transfer, std::span<unsigned char> page) {
    const auto kRead = read_chunk(transfer.input, transfer.position, page);
    if (!kRead.ok()) {
        return kRead.error();
    }
    if (kRead.value() == 0) {
        return 0UL;
    }
    if (kRead.value() > page.size()) {
        return base::KernelError::kIOError;
    }
    const auto kPut = write_chunk(transfer.output, {page.data(), kRead.value()});
    if (!kPut.ok()) {
        if (kPut.error() == base::KernelError::kBrokenPipe) {
            proc::SendSignal(*CurrentTask(), kSigPipe);
        }
        return kPut.error();
    }
    if (kPut.value() > kRead.value()) {
        return base::KernelError::kIOError;
    }
    transfer.position += kPut.value();
    transfer.remaining -= kPut.value();
    if (transfer.seekable_output) {
        transfer.output.offset += kPut.value();
    }
    if (transfer.shared_input) {
        transfer.input.offset = transfer.position;
    }
    return kPut.value();
}

long long transfer_bytes(Transfer& transfer) {
    unsigned char page[arch::page::kSize];
    unsigned long total = 0;
    while (transfer.remaining != 0) {
        if (proc::HasPendingSignal(*CurrentTask())) {
            return total != 0 ? static_cast<long long>(total) : -kEintr;
        }
        const auto kWant  = base::math::Min(transfer.remaining, sizeof(page));
        const auto kMoved = transfer_chunk(transfer, {page, kWant});
        if (!kMoved.ok()) {
            return total != 0 ? static_cast<long long>(total) : -ErrnoFrom(kMoved.error());
        }
        total += kMoved.value();
        if (kMoved.value() < kWant) {
            break;
        }
    }
    return static_cast<long long>(total);
}

long long sendfile_run(const SyscallFrame& frame, long long& position) {
    const auto kInput  = CurrentTask()->files->shared(frame.rsi);
    const auto kOutput = CurrentTask()->files->shared(frame.rdi);
    if (!kInput.ok() || !kOutput.ok()) {
        return -kEbadf;
    }
    auto& input  = *kInput.value();
    auto& output = *kOutput.value();
    if (input.ops == nullptr || output.ops == nullptr) {
        return -kEbadf;
    }
    const auto kInputMode  = input.status_flags & kOpenAccMode;
    const auto kOutputMode = output.status_flags & kOpenAccMode;
    if (kInputMode == kOpenWriteOnly || kInputMode > kOpenReadWrite ||
        kOutputMode > kOpenReadWrite || (output.status_flags & kOpenAccMode) == 0 ||
        input.ops->read == nullptr || output.ops->write == nullptr) {
        return -kEbadf;
    }
    const auto kRegularInput  = regular_file(input);
    const auto kRegularOutput = regular_file(output);
    if (!kRegularInput.ok() || !kRegularOutput.ok()) {
        return -kEio;
    }
    if (!kRegularInput.value()) {
        return frame.rdx != 0 ? -kEspipe : -kEinval;
    }
    if ((output.status_flags & kOpenAppend) != 0) {
        return -kEinval;
    }
    if (frame.rdx == 0) {
        position = static_cast<long long>(input.offset);
    }
    constexpr auto kMaxOffset = std::numeric_limits<long long>::max();
    if (position < 0 || frame.r10 > static_cast<unsigned long long>(kMaxOffset)) {
        return -kEinval;
    }
    if (kRegularOutput.value() && output.offset > static_cast<unsigned long>(kMaxOffset)) {
        return -kEinval;
    }
    const auto kMaxTransfer = base::math::AlignDown(
        static_cast<unsigned long>(std::numeric_limits<int>::max()), arch::page::kSize);
    auto       count       = base::math::Min(static_cast<unsigned long>(frame.r10), kMaxTransfer);
    const auto kInputRoom  = static_cast<unsigned long>(kMaxOffset - position);
    const auto kOutputRoom = kRegularOutput.value()
                                 ? static_cast<unsigned long>(kMaxOffset) - output.offset
                                 : static_cast<unsigned long>(kMaxOffset);
    const auto kRoom       = base::math::Min(kInputRoom, kOutputRoom);
    if (count != 0 && kRoom == 0) {
        return -kEoverflow;
    }
    count = base::math::Min(count, kRoom);
    Transfer transfer{
        .input           = input,
        .output          = output,
        .position        = static_cast<unsigned long>(position),
        .remaining       = count,
        .shared_input    = frame.rdx == 0,
        .seekable_output = kRegularOutput.value(),
    };
    const auto kResult = transfer_bytes(transfer);
    position           = static_cast<long long>(transfer.position);
    return kResult;
}

}

long long HandleSendfile(const SyscallFrame& frame) {
    long long position = 0;
    if (frame.rdx != 0 && !uaccess::CopyFromUser(&position, frame.rdx, sizeof(position))) {
        return -kEfault;
    }
    const auto kResult = sendfile_run(frame, position);
    if (frame.rdx != 0 && !uaccess::CopyToUser(frame.rdx, &position, sizeof(position))) {
        return -kEfault;
    }
    return kResult;
}

}
