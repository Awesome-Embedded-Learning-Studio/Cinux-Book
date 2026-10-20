#include "kernel/syscall/sys_exec.hpp"

#include <cstdint>
#include <string_view>
#include <utility>

#include "cinux/math.hpp"
#include "cinux/memory.hpp"
#include "cinux/own.hpp"
#include "cinux/path.hpp"
#include "cinux/ptr.hpp"
#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/file_world.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/loader/loader.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vma.hpp"
#include "kernel/proc/proc_config.hpp"
#include "kernel/proc/signals.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

namespace {

constexpr unsigned long long kUserFlags = 0x202;

/// How many argument or environment runs one exec may carry, and how
/// many bytes of their strings; the entry picture lives in one page and
/// the words alone claim a tenth of it, so the budgets stay loud.
constexpr unsigned long kMaxEntryWords  = 16;
constexpr unsigned long kEntryTextBytes = 512;

/// The one environment run a caller with no environment still gets:
/// where PATH searches look, so a hosted shell finds /bin without
/// falling back on its compiled-in guess.
char g_default_path[] = "PATH=/bin:/usr/bin";

/// Copies one null-terminated user run into the staging pool; false
/// when the run walks out of the user half or out of budget.
bool stage_user_run(unsigned long address, char* pool, unsigned long& pool_used, char*& landed) {
    if (address >= kUserAddressLimit) {
        return false;
    }
    auto const* const kText =
        reinterpret_cast<const char*>(address);  // NOLINT(performance-no-int-to-ptr)
    unsigned long length = 0;
    while (length < kEntryTextBytes - pool_used && kText[length] != '\0') {
        ++length;
    }
    if (kText[length] != '\0') {
        return false;
    }
    landed = pool + pool_used;
    for (unsigned long index = 0; index <= length; ++index) {
        landed[index] = kText[index];
    }
    pool_used += length + 1;
    return true;
}

/// Stages one user pointer array into kernel terms; counts the run
/// first, then copies, refusing arrays longer than the budget.
bool stage_user_vector(unsigned long user_words, char* pool, unsigned long& pool_used,
                       char** staged) {
    if (user_words == 0) {
        return true;
    }
    if (!UserRangeReadable(user_words, (kMaxEntryWords + 1) * sizeof(void*))) {
        return false;
    }
    auto const* const kWords =
        reinterpret_cast<const unsigned long*>(user_words);  // NOLINT(performance-no-int-to-ptr)
    unsigned long total = 0;
    while (total <= kMaxEntryWords && kWords[total] != 0) {
        ++total;
    }
    if (total > kMaxEntryWords) {
        return false;
    }
    for (unsigned long index = 0; index < total; ++index) {
        if (!stage_user_run(kWords[index], pool, pool_used, staged[index])) {
            return false;
        }
    }
    return true;
}

}  // namespace

long long HandleExecve(const SyscallFrame& frame) {
    char            path[fs::kFsPathMax] = {};
    const long long kFolded              = FoldUserPath(frame.rdi, path, sizeof(path));
    if (kFolded < 0) {
        return kFolded;
    }
    const base::Result<fs::FsView> kView = fs::FileWorld::self().resolve(path);
    if (!kView.ok()) {
        return -kEnoent;
    }
    unsigned long              program_size = 0;
    base::Own<unsigned char[]> program;
    const base::Result<void>   kRead = fs::ReadWholeFile(kView.value(), program, program_size);
    if (!kRead.ok()) {
        return -ErrnoFrom(kRead.error());
    }
    proc::Task* const              kSelf = CurrentTask();
    const base::Result<fs::FsNode> kNode =
        kView.value().ops->lookup(kView.value().self, kView.value().rest);
    if (!kNode.ok()) {
        return -kEnoent;
    }
    base::Shared<mm::AddressSpace> fresh(new mm::AddressSpace{});
    base::Shared<mm::VmaStore>     ranges(new mm::VmaStore{});
    std::uint64_t                  entry = 0;
    if (!fresh.holds() || !ranges.holds() || !fresh->init()) {
        return -kEnomem;
    }
    char          argument_pool[kEntryTextBytes] = {};
    char*         argv_words[kMaxEntryWords + 1] = {};
    char*         envp_words[kMaxEntryWords + 1] = {};
    unsigned long pool_used                      = 0;
    if (frame.rsi == 0) {
        argv_words[0] = path;
    } else if (!stage_user_vector(frame.rsi, argument_pool, pool_used, argv_words)) {
        return -kE2big;
    }
    if (frame.rdx != 0) {
        if (!stage_user_vector(frame.rdx, argument_pool, pool_used, envp_words)) {
            return -kE2big;
        }
    } else {
        envp_words[0] = g_default_path;
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
    cinux::base::SetBytes(cinux::base::PtrAt<unsigned char>(mm::DirectMapVirt(kStackFrame.raw)), 0,
                          cinux::arch::page::kSize);
    if (!fresh->map_user(proc::kUserProgramStackTop - cinux::arch::page::kSize, kStackFrame.raw)) {
        mm::Pmm::self().free_page(kStackFrame);
        return -kEnomem;
    }
    if (!proc::MapSignalTrampoline(*fresh, *ranges)) {
        return -kEnomem;
    }
    kSelf->signals         = proc::SignalState{};
    kSelf->clear_child_tid = 0;
    kSelf->files->close_on_exec();
    std::string_view comm_parent = {};
    std::string_view comm_leaf   = {};
    static_cast<void>(base::SplitLeaf(path, comm_parent, comm_leaf));
    const std::string_view kExePath   = path;
    unsigned long long     name_index = 0;
    base::SetBytes(kSelf->comm, 0, sizeof(kSelf->comm));
    while (name_index < comm_leaf.size() && name_index < sizeof(kSelf->comm) - 1) {
        kSelf->comm[name_index] = comm_leaf[name_index];
        ++name_index;
    }
    base::SetBytes(kSelf->exe, 0, sizeof(kSelf->exe));
    unsigned long long exe_index = 0;
    while (exe_index < kExePath.size() && exe_index < sizeof(kSelf->exe) - 1) {
        kSelf->exe[exe_index] = kExePath[exe_index];
        ++exe_index;
    }
    // The dying worlds must not come apart while CR3 still walks them:
    // park the old holds aside, switch, and let them go at scope end.
    // NOLINTNEXTLINE(misc-const-correctness) the dying worlds go at scope end
    base::Shared<mm::AddressSpace> old_space  = std::move(kSelf->space);
    // NOLINTNEXTLINE(misc-const-correctness) the dying worlds go at scope end
    base::Shared<mm::VmaStore>     old_ranges = std::move(kSelf->vmas);
    kSelf->space                              = std::move(fresh);
    kSelf->vmas                               = std::move(ranges);
    kSelf->user_root                          = kSelf->space->root();
    kSelf->space->activate();
    auto& mutable_frame = const_cast<SyscallFrame&>(frame);
    if (loader::WriteEntryPicture(
            cinux::base::PtrAt<unsigned char>(mm::DirectMapVirt(kStackFrame.raw)),
            proc::kUserProgramStackTop - proc::kUserEntryStackOffset, argv_words,
            envp_words) == 0) {
        return -kE2big;
    }
    mutable_frame.user_rsp = proc::kUserProgramStackTop - proc::kUserEntryStackOffset;
    mutable_frame.rcx      = entry;
    mutable_frame.r11      = kUserFlags;
    mutable_frame.rax      = 0;
    return 0;
}

}  // namespace cinux::syscall
