#include "kernel/driver/nvme/nvme.hpp"

#include <array>
#include <atomic>
#include <cstdint>

#include "cinux/addr.hpp"
#include "cinux/bit_ops/bitmask.hpp"
#include "cinux/byte_order.hpp"
#include "cinux/memory.hpp"
#include "cinux/ptr.hpp"
#include "kernel/arch/x86_64/instructions.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/boot/print.hpp"
#include "kernel/driver/block/block_device_concept.hpp"
#include "kernel/driver/hpet/hpet.hpp"
#include "kernel/driver/nvme/nvme_config.hpp"
#include "kernel/driver/nvme/nvme_layout.hpp"
#include "kernel/driver/pci/pci.hpp"
#include "kernel/driver/pci/pci_layout.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/sync.hpp"

namespace cinux::driver {
namespace {

using Register                           = base::bit::BitMask<std::uint32_t>;
constexpr Register      kEnable          = base::bit::MaskBit<std::uint32_t>(0);
constexpr Register      kFatal           = base::bit::MaskBit<std::uint32_t>(1);
constexpr unsigned long kCapabilities    = 0;
constexpr unsigned long kInterruptMask   = 0x0C;
constexpr unsigned long kConfiguration   = 0x14;
constexpr unsigned long kStatus          = 0x1C;
constexpr unsigned long kAdminAttributes = 0x24;
constexpr unsigned long kAdminSubmission = 0x28;
constexpr unsigned long kAdminCompletion = 0x30;
constexpr unsigned long kDoorbells       = 0x1000;

struct Queue {
    unsigned long submission{};
    unsigned long completion{};
    std::uint16_t tail{};
    std::uint16_t head{};
    std::uint16_t next_identity{};
    std::uint16_t identity{};
    bool          phase = true;
};

unsigned long take_page() {
    const auto kFrame = mm::Pmm::self().allocate_page();
    if (kFrame != base::PhysAddr{}) {
        base::SetBytes(base::PtrAt<void>(mm::DirectMapVirt(kFrame.raw)), 0, arch::page::kSize);
    }
    return kFrame.raw;
}

void put_address(NvmeCommand& command, unsigned long index, std::uint64_t physical) {
    command.words[index]     = static_cast<std::uint32_t>(physical);
    command.words[index + 1] = static_cast<std::uint32_t>(physical >> 32U);
}

}  // namespace

struct Nvme::State {
    unsigned long     registers{};
    unsigned long     stride{};
    unsigned long     data{};
    std::uint64_t     blocks{};
    unsigned long     sector_bytes{};
    std::uint32_t     namespace_id{};
    Queue             admin{};
    Queue             io{.identity = 1};
    proc::Mutex       gate;
    std::atomic<bool> online;
    bool              scratch{};

    [[nodiscard]] std::uint32_t read(unsigned long offset) const {
        return *base::PtrAt<volatile std::uint32_t>(registers + offset);
    }

    void write(unsigned long offset, std::uint32_t value) const {
        *base::PtrAt<volatile std::uint32_t>(registers + offset) = value;
    }

    void address(unsigned long offset, unsigned long physical) const {
        write(offset, static_cast<std::uint32_t>(physical));
        write(offset + 4, static_cast<std::uint32_t>(physical >> 32U));
    }

    [[nodiscard]] bool wait_ready(bool expected) const {
        const auto kDeadline = Hpet::self().since_boot_ns() + kNvmeTimeoutNs;
        while (Hpet::self().since_boot_ns() < kDeadline) {
            const Register kState{read(kStatus)};
            if (kState.has(kFatal)) {
                return false;
            }
            if (kState.has(kEnable) == expected) {
                return true;
            }
            arch::CpuRelax();
        }
        return false;
    }

    bool submit(Queue& queue, NvmeCommand command) {
        const auto kIdentity = ++queue.next_identity;
        Register   header{command.words[0]};
        header.deposit({.low = 16, .width = 16}, kIdentity);
        command.words[0]       = header.raw;
        auto* submission       = base::PtrAt<NvmeCommand>(mm::DirectMapVirt(queue.submission));
        submission[queue.tail] = command;
        queue.tail             = static_cast<std::uint16_t>((queue.tail + 1) % kNvmeQueueEntries);
        std::atomic_thread_fence(std::memory_order_release);
        write(kDoorbells + (2UL * queue.identity * stride), queue.tail);
        const auto* completion =
            base::PtrAt<volatile NvmeCompletion>(mm::DirectMapVirt(queue.completion));
        const auto kDeadline = Hpet::self().since_boot_ns() + kNvmeTimeoutNs;
        while (Hpet::self().since_boot_ns() < kDeadline) {
            const auto&    entry = completion[queue.head];
            const Register kFlags{entry.status};
            if (kFlags.extract({.low = 0, .width = 1}) == static_cast<unsigned int>(queue.phase)) {
                std::atomic_thread_fence(std::memory_order_acquire);
                const NvmeCompletion kTaken{
                    .result          = entry.result,
                    .reserved        = entry.reserved,
                    .submission_head = entry.submission_head,
                    .queue           = entry.queue,
                    .identity        = entry.identity,
                    .status          = entry.status,
                };
                const bool kPassed = NvmeCompleted(kTaken, kIdentity, queue.identity, queue.phase);
                queue.head = static_cast<std::uint16_t>((queue.head + 1) % kNvmeQueueEntries);
                if (queue.head == 0) {
                    queue.phase = !queue.phase;
                }
                write(kDoorbells + (((2UL * queue.identity) + 1UL) * stride), queue.head);
                if (kTaken.identity != kIdentity || kTaken.queue != queue.identity) {
                    online.store(false, std::memory_order_release);
                    write(kConfiguration, read(kConfiguration) & ~kEnable.raw);
                }
                if (!kPassed) {
                    print::Println("[nvme] refused completion status=%X cid=%u wanted=%u queue=%u",
                                   kTaken.status, kTaken.identity, kIdentity, kTaken.queue);
                }
                return kPassed;
            }
            if (Register{read(kStatus)}.has(kFatal)) {
                break;
            }
            if (proc::Scheduler::self().current() != nullptr) {
                proc::Scheduler::self().yield();
            } else {
                arch::CpuRelax();
            }
        }
        online.store(false, std::memory_order_release);
        write(kConfiguration, read(kConfiguration) & ~kEnable.raw);
        print::Println("[nvme] command timed out, controller quarantined");
        return false;
    }

    bool identify(std::uint32_t selector, std::uint32_t name) {
        base::SetBytes(base::PtrAt<void>(mm::DirectMapVirt(data)), 0, arch::page::kSize);
        NvmeCommand command{};
        command.words[0] = 6;
        command.words[1] = name;
        put_address(command, 6, data);
        command.words[10] = selector;
        return submit(admin, command);
    }

    bool transfer(bool writing, Lba first, BlockSpan span, void* buffer) {
        const auto kGuard = gate.guard();
        if (!online.load(std::memory_order_acquire) || span.value == 0 ||
            span.value > arch::page::kSize / sector_bytes || first.value >= blocks ||
            span.value > blocks - first.value) {
            return false;
        }
        const auto kBytes = span.value * sector_bytes;
        auto*      bounce = base::PtrAt<unsigned char>(mm::DirectMapVirt(data));
        if (writing) {
            base::CopyBytes(bounce, buffer, kBytes);
        }
        NvmeCommand command{};
        command.words[0] = writing ? 1 : 2;
        command.words[1] = namespace_id;
        put_address(command, 6, data);
        put_address(command, 10, first.value);
        command.words[12] = span.value - 1U;
        if (!submit(io, command)) {
            return false;
        }
        if (!writing) {
            base::CopyBytes(buffer, bounce, kBytes);
        }
        return true;
    }
};

bool Nvme::init() {
    const auto* device = PciBus::self().find_by_class(PciClassCode::kMassStorage, kNvmeSubclass);
    if (device == nullptr) {
        print::Println("[nvme] absent");
        return false;
    }
    if (!Hpet::self().present() || state_ != nullptr) {
        return ready();
    }
    const auto kBar = BarBase(device->bars, 0);
    if (kBar == 0 || !mm::MapDeviceWindow(kBar, 0x2000)) {
        print::Println("[nvme] firmware left an unusable BAR");
        return false;
    }
    PciBus::self().enable_device(*device);
    state_ = new State{};
    if (state_ == nullptr) {
        return false;
    }
    auto& state     = *state_;
    state.registers = mm::IoremapVirt(kBar);
    const base::bit::BitMask<std::uint64_t> kCap{
        state.read(kCapabilities) |
        (static_cast<std::uint64_t>(state.read(kCapabilities + 4)) << 32U)};
    if (kCap.extract({.low = 0, .width = 16}) + 1 < kNvmeQueueEntries ||
        kCap.extract({.low = 48, .width = 4}) != 0 || kCap.extract({.low = 37, .width = 1}) == 0) {
        return false;
    }
    state.stride = 4UL << kCap.extract({.low = 32, .width = 4});
    if (!mm::MapDeviceWindow(kBar, kDoorbells + (4 * state.stride))) {
        return false;
    }
    state.write(kConfiguration, state.read(kConfiguration) & ~kEnable.raw);
    if (!state.wait_ready(false)) {
        return false;
    }
    state.admin.submission = take_page();
    state.admin.completion = take_page();
    state.io.submission    = take_page();
    state.io.completion    = take_page();
    state.data             = take_page();
    if (state.admin.submission == 0 || state.admin.completion == 0 || state.io.submission == 0 ||
        state.io.completion == 0 || state.data == 0) {
        return false;
    }
    Register attributes{};
    attributes.deposit({.low = 0, .width = 12}, kNvmeQueueEntries - 1U);
    attributes.deposit({.low = 16, .width = 12}, kNvmeQueueEntries - 1U);
    state.write(kAdminAttributes, attributes.raw);
    state.address(kAdminSubmission, state.admin.submission);
    state.address(kAdminCompletion, state.admin.completion);
    state.write(kInterruptMask, ~0U);
    Register configuration{kEnable};
    configuration.deposit({.low = 16, .width = 4}, 6);
    configuration.deposit({.low = 20, .width = 4}, 4);
    state.write(kConfiguration, configuration.raw);
    if (!state.wait_ready(true) || !state.identify(1, 0)) {
        return false;
    }
    const auto* bytes = base::PtrAt<const unsigned char>(mm::DirectMapVirt(state.data));
    state.scratch     = base::EqualBytes(bytes + 4, "cinux-nvme", 10);
    if (!state.identify(2, 0)) {
        return false;
    }
    state.namespace_id = base::ReadWord32(bytes, 0);
    if (state.namespace_id == 0 || !state.identify(0, state.namespace_id)) {
        return false;
    }
    state.blocks         = base::ReadWord64(bytes, 0);
    const auto kFormat   = Register{bytes[26]}.extract({.low = 0, .width = 4});
    const auto kEntry    = 128U + (4U * kFormat);
    const auto kExponent = bytes[kEntry + 2];
    if (base::ReadWord16(bytes, kEntry) != 0 || kExponent < 9 || kExponent > 12 ||
        state.blocks == 0) {
        return false;
    }
    state.sector_bytes = 1UL << kExponent;
    NvmeCommand completion{};
    completion.words[0] = 5;
    put_address(completion, 6, state.io.completion);
    Register queue_size{};
    queue_size.deposit({.low = 0, .width = 16}, 1);
    queue_size.deposit({.low = 16, .width = 16}, kNvmeQueueEntries - 1U);
    completion.words[10] = queue_size.raw;
    completion.words[11] = 1;
    NvmeCommand submission{};
    submission.words[0] = 1;
    put_address(submission, 6, state.io.submission);
    submission.words[10] = queue_size.raw;
    Register paired_queue{1};
    paired_queue.deposit({.low = 16, .width = 16}, 1);
    submission.words[11] = paired_queue.raw;
    if (!state.submit(state.admin, completion) || !state.submit(state.admin, submission)) {
        return false;
    }
    state.online.store(true, std::memory_order_release);
    print::Println("[nvme] namespace %u online, %llu blocks of %lu bytes", state.namespace_id,
                   state.blocks, state.sector_bytes);
    return true;
}

bool Nvme::ready() const {
    return state_ != nullptr && state_->online.load(std::memory_order_acquire);
}

bool Nvme::scratch_namespace() const {
    return ready() && state_->scratch;
}

bool Nvme::read_blocks(Lba first, BlockSpan span, std::uint8_t* sink) {
    return state_ != nullptr && state_->transfer(false, first, span, sink);
}

bool Nvme::write_blocks(Lba first, BlockSpan span, const std::uint8_t* source) {
    return state_ != nullptr &&
           state_->transfer(true, first, span, const_cast<std::uint8_t*>(source));
}

std::uint64_t Nvme::block_count() const {
    return state_ != nullptr ? state_->blocks : 0;
}
unsigned long Nvme::block_size() const {
    return state_ != nullptr ? state_->sector_bytes : 0;
}

bool Nvme::flush() {
    if (state_ == nullptr) {
        return false;
    }
    const auto  kGuard = state_->gate.guard();
    NvmeCommand command{};
    command.words[1] = state_->namespace_id;
    return ready() && state_->submit(state_->io, command);
}

bool Nvme::verify_io() {
    if (!scratch_namespace()) {
        return false;
    }
    std::array<std::uint8_t, arch::page::kSize> saved{};
    std::array<std::uint8_t, arch::page::kSize> payload{};
    const Lba                                   kLast{.value = block_count() - 1};
    const BlockSpan                             kOne{.value = 1};
    if (!read_blocks(kLast, kOne, saved.data())) {
        return false;
    }
    bool passed = true;
    for (unsigned long round = 0; round < 20; ++round) {
        base::SetBytes(payload.data(), static_cast<unsigned char>(round + 1), block_size());
        passed = passed && write_blocks(kLast, kOne, payload.data());
        base::SetBytes(payload.data(), 0, block_size());
        passed = passed && read_blocks(kLast, kOne, payload.data());
        for (unsigned long index = 0; index < block_size(); ++index) {
            passed = passed && payload[index] == round + 1;
        }
    }
    const bool kRestored = write_blocks(kLast, kOne, saved.data()) && flush();
    print::Println("[nvme] queue wrap and write-readback %s",
                   passed && kRestored ? "PASS" : "FAIL");
    return passed && kRestored;
}

}  // namespace cinux::driver
