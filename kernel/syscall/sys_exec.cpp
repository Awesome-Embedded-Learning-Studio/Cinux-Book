#include "kernel/syscall/sys_exec.hpp"

#include <cstdint>

#include "cinux/math.hpp"
#include "cinux/own.hpp"
#include "cinux/ptr.hpp"
#include "cinux/result.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/arch/x86_64/per_cpu.hpp"
#include "kernel/fs/file_world.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/loader/loader.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vma.hpp"
#include "kernel/proc/proc_config.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

namespace {

constexpr unsigned long long kUserFlags = 0x202;

}  // namespace

long long HandleExecve(Word path_word, Word /*second*/, Word /*third*/) {
    char            path[fs::kFsPathMax] = {};
    const long long kFolded              = FoldUserPath(path_word, path, sizeof(path));
    if (kFolded < 0) {
        return kFolded;
    }
    const base::Result<fs::FsView> kView = fs::FileWorld::self().resolve(path);
    if (!kView.ok()) {
        return -kEnoent;
    }
    unsigned long              program_size = 0;
    base::Own<unsigned char[]> program;
    if (!fs::ReadWholeFile(kView.value(), program, program_size)) {
        return -kEio;
    }
    proc::Task* const              kSelf     = CurrentTask();
    mm::AddressSpace* const        kOldWorld = kSelf->space;  // NOLINT(misc-const-correctness)
    const base::Result<fs::FsNode> kNode =
        kView.value().ops->lookup(kView.value().self, kView.value().rest);
    if (!kNode.ok()) {
        return -kEnoent;
    }
    base::Own<mm::AddressSpace> fresh(new mm::AddressSpace{});
    base::Own<mm::VmaStore>     ranges(new mm::VmaStore{});
    std::uint64_t               entry = 0;
    if (!fresh.holds() || !ranges.holds() || !fresh->init()) {
        return -kEnomem;
    }
    loader::LoadWorld world{
        .space     = *fresh,
        .vmas      = *ranges,
        .file      = kNode.value(),
        .file_ops  = kView.value().ops,
        .file_self = kView.value().self,
    };
    if (!loader::LoadElfIntoSpace(program.get(), program_size, world, entry)) {
        return -kEnomem;
    }
    ranges->seed_heap(cinux::base::math::AlignUp(ranges->max_end(), cinux::arch::page::kSize),
                      mm::kUserBrkMmapSplit);
    mm::VmaRegion stack_range{};
    stack_range.start = proc::kUserProgramStackTop - mm::kUserStackBytes - cinux::arch::page::kSize;
    stack_range.end   = proc::kUserProgramStackTop;
    stack_range.kind  = mm::VmaKind::kStack;
    stack_range.readable = true;
    stack_range.writable = true;
    if (!ranges->register_range(stack_range).ok()) {
        return -kEnomem;
    }
    const mm::Pmm::PhysAddr kStackFrame = mm::Pmm::self().allocate_page();
    if (kStackFrame == mm::Pmm::PhysAddr{}) {
        return -kEnomem;
    }
    if (!fresh->map_user(proc::kUserProgramStackTop - cinux::arch::page::kSize, kStackFrame.raw)) {
        mm::Pmm::self().free_page(kStackFrame);
        return -kEnomem;
    }
    kSelf->space     = fresh.release();
    kSelf->vmas      = ranges.release();
    kSelf->user_root = kSelf->space->root();
    kSelf->space->activate();
    delete kOldWorld;
    auto* const kFrame =
        cinux::base::PtrAt<SyscallFrame>(arch::per_cpu::PerCpu::self().syscall_frame);
    kFrame->user_rsp = proc::kUserProgramStackTop - 8;
    kFrame->rcx      = entry;
    kFrame->r11      = kUserFlags;
    kFrame->rax      = 0;
    return 0;
}

}  // namespace cinux::syscall
