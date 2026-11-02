#include "kernel/fs/procfs/procfs_init.hpp"

#include <cstdint>
#include <span>

#include "cinux/bit_ops/bitmask.hpp"
#include "cinux/memory.hpp"
#include "cinux/result.hpp"
#include "cinux/text.hpp"
#include "kernel/arch/x86_64/cpuid.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/arch/x86_64/smp.hpp"
#include "kernel/driver/device/device.hpp"
#include "kernel/fs/file_world.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/procfs/procfs.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/net/ifaces.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/proc/task_snapshot.hpp"
#include "kernel/syscall/sys_clock.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::fs {

namespace {

/// One device class's name, the seam's shared spelling.
const char* class_name_of(driver::DeviceClass klass) {
    switch (klass) {
    case driver::DeviceClass::kCharacter:
        return kProcClassCharacter;
    case driver::DeviceClass::kBlock:
        return kProcClassBlock;
    default:
        return kProcClassNetwork;
    }
}

bool source_nth_tgid(void* /*context*/, unsigned long ordinal, unsigned long& tgid) {
    unsigned long long pid = 0;
    if (!proc::Scheduler::self().nth_group_leader(ordinal, pid)) {
        return false;
    }
    tgid = static_cast<unsigned long>(pid);
    return true;
}

bool source_snapshot(void* /*context*/, unsigned long tgid, proc::TaskSnapshot& into) {
    return proc::Scheduler::self().snapshot_by_pid(tgid, into);
}

unsigned long source_exe_path(void* /*context*/, char* out, unsigned long cap) {
    const proc::Task* const kSelf = cinux::syscall::CurrentTask();
    unsigned long           used  = 0;
    while (used + 1 < cap && kSelf->exe[used] != '\0') {
        out[used] = kSelf->exe[used];
        ++used;
    }
    out[used] = '\0';
    return used;
}

/// Resolves the current task's program to its backend view and node;
/// false when the file has gone.
bool resolve_exe(FsView& view, FsNode& node) {
    char path[fs::kFsPathMax] = {};
    if (source_exe_path(nullptr, path, sizeof(path)) == 0) {
        return false;
    }
    const base::Result<FsView> kView = FileWorld::self().resolve(path);
    if (!kView.ok()) {
        return false;
    }
    view                             = kView.value();
    const base::Result<FsNode> kNode = view.ops->lookup(view.self, view.rest);
    if (!kNode.ok()) {
        return false;
    }
    node = kNode.value();
    return true;
}

// NOLINTNEXTLINE(readability-function-size) the source seat names five
bool source_exe_bytes(void* /*context*/, unsigned long offset, void* sink, unsigned long count,
                      unsigned long& gave) {
    FsView view{};
    FsNode node{};
    if (!resolve_exe(view, node)) {
        return false;
    }
    const base::Result<unsigned long> kGot = view.ops->read(view.self, node, offset, sink, count);
    if (!kGot.ok()) {
        return false;
    }
    gave = kGot.value();
    return true;
}

unsigned long source_exe_size(void* /*context*/) {
    FsView view{};
    FsNode node{};
    if (!resolve_exe(view, node)) {
        return 0;
    }
    FsStat                   probed{};
    const base::Result<void> kProbed = view.ops->stat(view.self, node, probed);
    if (!kProbed.ok()) {
        return 0;
    }
    return probed.size;
}

void source_memory(void* /*context*/, unsigned long& free_pages, unsigned long& total_pages) {
    free_pages  = mm::Pmm::self().free_page_count();
    total_pages = mm::Pmm::self().managed_page_count();
}

bool source_nth_cpu(void* /*context*/, unsigned long ordinal, ProcCpuFacts& into) {
    if (ordinal >= arch::smp::OnlineCpuCount()) {
        return false;
    }
    into               = ProcCpuFacts{};
    into.processor     = ordinal;
    into.apic_id       = arch::smp::Mechanisms(static_cast<unsigned int>(ordinal)).apic_id;
    const auto kVendor = arch::ReadCpuId(0, 0);
    base::CopyBytes(into.vendor_id, &kVendor.ebx, sizeof(kVendor.ebx));
    base::CopyBytes(into.vendor_id + sizeof(kVendor.ebx), &kVendor.edx, sizeof(kVendor.edx));
    base::CopyBytes(into.vendor_id + sizeof(kVendor.ebx) + sizeof(kVendor.edx), &kVendor.ecx,
                    sizeof(kVendor.ecx));
    if (kVendor.eax >= 1) {
        const auto kIdentity = arch::ReadCpuId(1, 0);
        using Word           = base::bit::BitMask<std::uint32_t>;
        constexpr base::bit::BitRange kStepping{.low = 0, .width = 4};
        constexpr base::bit::BitRange kBaseModel{.low = 4, .width = 4};
        constexpr base::bit::BitRange kBaseFamily{.low = 8, .width = 4};
        constexpr base::bit::BitRange kExtendedModel{.low = 16, .width = 4};
        constexpr base::bit::BitRange kExtendedFamily{.low = 20, .width = 8};
        const Word                    kSignature{kIdentity.eax};
        const auto                    kFamily = kSignature.extract(kBaseFamily);
        into.family = kFamily == 15 ? kFamily + kSignature.extract(kExtendedFamily) : kFamily;
        Word model{kSignature.extract(kBaseModel)};
        if (kFamily == 6 || kFamily == 15) {
            model.deposit(kBaseModel, kSignature.extract(kExtendedModel));
        }
        into.model    = model.raw;
        into.stepping = kSignature.extract(kStepping);
        struct CpuFlag {
            unsigned char position;
            const char*   name;
        };
        constexpr CpuFlag kFlags[] = {
            {.position = 0, .name = "fpu"},   {.position = 4, .name = "tsc"},
            {.position = 9, .name = "apic"},  {.position = 12, .name = "mtrr"},
            {.position = 15, .name = "cmov"}, {.position = 22, .name = "acpi"},
            {.position = 23, .name = "mmx"},  {.position = 24, .name = "fxsr"},
            {.position = 25, .name = "sse"},  {.position = 26, .name = "sse2"},
        };
        const Word    kFeatures{kIdentity.edx};
        unsigned long used = 0;
        for (const auto& flag : kFlags) {
            if (kFeatures.has(base::bit::MaskBit<std::uint32_t>(flag.position))) {
                if (used != 0) {
                    into.flags[used++] = ' ';
                }
                used += base::CopyText(std::span{into.flags}.subspan(used), flag.name);
            }
        }
    }
    if (arch::ReadCpuId(0x80000000U, 0).eax >= 0x80000004U) {
        for (unsigned int leaf = 0; leaf < 3; ++leaf) {
            const auto         kBrand   = arch::ReadCpuId(0x80000002U + leaf, 0);
            const unsigned int kWords[] = {kBrand.eax, kBrand.ebx, kBrand.ecx, kBrand.edx};
            base::CopyBytes(into.model_name + (leaf * sizeof(kWords)), kWords, sizeof(kWords));
        }
    }
    return true;
}

bool source_nth_device(void* /*context*/, unsigned long ordinal, ProcDeviceFacts& into) {
    driver::DeviceFacts facts{};
    if (!driver::DeviceModel::self().nth_device(ordinal, facts)) {
        return false;
    }
    into        = ProcDeviceFacts{};
    into.number = facts.number;
    base::CopyText(into.name, facts.name);
    base::CopyText(into.class_name, class_name_of(facts.klass));
    return true;
}

bool source_nth_net_iface(void* /*context*/, unsigned long ordinal, char* out) {
    return net::NameInterfaceAt(ordinal, out);
}

}  // namespace

void WireKernelProcSource(ProcFs& procfs) {
    const ProcSource kSource{
        .mounts =
            [](auto*, char* out, unsigned long capacity) {
                return FileWorld::self().mounts_text(out, capacity);
            },
        .uptime_ns         = [](auto*) { return syscall::MonotonicNanoseconds(); },
        .nth_tgid          = source_nth_tgid,
        .snapshot          = source_snapshot,
        .exe_path          = source_exe_path,
        .exe_bytes         = source_exe_bytes,
        .exe_size          = source_exe_size,
        .memory            = source_memory,
        .memory_page_bytes = arch::page::kSize,
        .nth_device        = source_nth_device,
        .nth_net_iface     = source_nth_net_iface,
        .nth_cpu           = source_nth_cpu,
        .context           = nullptr,
    };
    procfs.set_source(kSource);
}

}  // namespace cinux::fs
