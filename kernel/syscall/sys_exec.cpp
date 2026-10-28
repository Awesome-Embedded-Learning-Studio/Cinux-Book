#include "kernel/syscall/sys_exec.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include "cinux/literal_types.hpp"
#include "cinux/math.hpp"
#include "cinux/memory.hpp"
#include "cinux/own.hpp"
#include "cinux/path.hpp"
#include "cinux/ptr.hpp"
#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/arch/x86_64/entropy.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/arch/x86_64/page_entry.hpp"
#include "kernel/arch/x86_64/rdrand.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/file_world.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/loader/elf_layout.hpp"
#include "kernel/loader/exec_arguments.hpp"
#include "kernel/loader/loader.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vma.hpp"
#include "kernel/proc/proc_config.hpp"
#include "kernel/proc/signals.hpp"
#include "kernel/proc/sync.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"
#include "kernel/syscall/uaccess.hpp"

namespace cinux::syscall {

namespace {

using base::operator""_KiB;

constexpr unsigned long long kUserFlags = 0x202;

/// Records the program's short and full names on the task that becomes it.
void note_program_identity(proc::Task& self, const char* path) {
    std::string_view comm_parent = {};
    std::string_view comm_leaf   = {};
    static_cast<void>(base::SplitLeaf(path, comm_parent, comm_leaf));
    const std::string_view kExePath   = path;
    unsigned long long     name_index = 0;
    base::SetBytes(self.comm, 0, sizeof(self.comm));
    while (name_index < comm_leaf.size() && name_index < sizeof(self.comm) - 1) {
        self.comm[name_index] = comm_leaf[name_index];
        ++name_index;
    }
    base::SetBytes(self.exe, 0, sizeof(self.exe));
    unsigned long long exe_index = 0;
    while (exe_index < kExePath.size() && exe_index < sizeof(self.exe) - 1) {
        self.exe[exe_index] = kExePath[exe_index];
        ++exe_index;
    }
}

/// Draws the sixteen bytes the entry picture offers as AT_RANDOM; a
/// machine without a working draw reports none and the picture omits
/// the entry.
const std::uint8_t* draw_entry_random(unsigned char (&bytes)[16]) {
    if (!arch::HasRdrand()) {
        return nullptr;
    }
    unsigned long long word  = 0;
    unsigned long      drawn = 0;
    if (arch::TryRdrand(word)) {
        cinux::base::CopyBytes(bytes, &word, sizeof(word));
        ++drawn;
    }
    if (arch::TryRdrand(word)) {
        cinux::base::CopyBytes(bytes + sizeof(word), &word, sizeof(word));
        ++drawn;
    }
    return drawn > 0 ? bytes : nullptr;
}

/// One load bias for the seat the program image takes: zero serves the
/// fixed-base shape whose link addresses already place it, the floor
/// plus a page-aligned slide serves the relocatable shape whose link
/// addresses start at zero.
unsigned long long image_load_bias(const unsigned char* program, std::size_t program_size) {
    if (!loader::ProgramNeedsLoadBias(program, program_size)) {
        return 0;
    }
    constexpr unsigned long long kExecSlideMask = 0xFF'000;
    return mm::kUserExecImageBase + (arch::RandomWord() & kExecSlideMask);
}

struct EntryStaging {
    base::Own<char[]>        text;
    base::Own<const char*[]> arguments;
    base::Own<const char*[]> environment;
    unsigned long            used{};
    unsigned long            capacity{};
};

long long count_user_vector(unsigned long address, unsigned long budget, unsigned long& count) {
    count = 0;
    if (address == 0) {
        return 0;
    }
    for (;;) {
        unsigned long word = 0;
        if (address >= kUserAddressLimit || count > (kUserAddressLimit - address) / sizeof(word) ||
            !uaccess::CopyFromUser(&word, address + (count * sizeof(word)), sizeof(word))) {
            return -kEfault;
        }
        if (word == 0) {
            return 0;
        }
        if (count >= loader::kMaxArgumentStrings || count >= budget / sizeof(word)) {
            return -kE2big;
        }
        ++count;
    }
}

long long stage_user_string(unsigned long address, EntryStaging& staged, const char*& landed) {
    const auto kBudget =
        base::math::Min(staged.capacity - staged.used, loader::kMaxArgumentStringBytes);
    unsigned long done = 0;
    while (done < kBudget) {
        constexpr unsigned long kCopyChunk = 64;
        if (address >= kUserAddressLimit || done >= kUserAddressLimit - address) {
            return -kEfault;
        }
        const auto kInPage = arch::page::kSize - ((address + done) % arch::page::kSize);
        const auto kChunk  = std::min({kCopyChunk, kBudget - done, kInPage});
        if (!uaccess::CopyFromUser(staged.text.get() + staged.used + done, address + done,
                                   kChunk)) {
            return -kEfault;
        }
        for (unsigned long index = done; index < done + kChunk; ++index) {
            if (staged.text[staged.used + index] == '\0') {
                landed = staged.text.get() + staged.used;
                staged.used += index + 1;
                return 0;
            }
        }
        done += kChunk;
    }
    return -kE2big;
}

long long stage_user_vector(unsigned long address, unsigned long count, EntryStaging& staged,
                            const char** vector) {
    for (unsigned long index = 0; index < count; ++index) {
        unsigned long word = 0;
        if (!uaccess::CopyFromUser(&word, address + (index * sizeof(word)), sizeof(word)) ||
            word == 0) {
            return -kEfault;
        }
        const auto kResult = stage_user_string(word, staged, vector[index]);
        if (kResult < 0) {
            return kResult;
        }
    }
    return 0;
}

long long stage_arguments(const SyscallFrame& frame, const char* path, EntryStaging& staged) {
    const auto    kStackLimit = CurrentTask()->limits[proc::kResourceStack][0];
    const auto    kTotal      = loader::ArgumentStringBudget(kStackLimit, 0, 0);
    unsigned long argc        = 0;
    unsigned long envc        = 0;
    auto          result      = count_user_vector(frame.rsi, kTotal.value(), argc);
    if (result < 0) {
        return result;
    }
    result = count_user_vector(frame.rdx, kTotal.value(), envc);
    if (result < 0) {
        return result;
    }
    const auto kBudget = loader::ArgumentStringBudget(kStackLimit, argc, envc);
    if (!kBudget.ok()) {
        return -kE2big;
    }
    staged.capacity = kBudget.value();
    staged.text.reset(new char[staged.capacity]);
    staged.arguments.reset(new const char*[base::math::Max(argc, 1UL) + 1]{});
    staged.environment.reset(new const char*[envc + 1]{});
    if (!staged.text.holds() || !staged.arguments.holds() || !staged.environment.holds()) {
        return -kEnomem;
    }
    unsigned long path_bytes = 0;
    while (path[path_bytes] != '\0') {
        ++path_bytes;
    }
    if (path_bytes + 1 > staged.capacity) {
        return -kE2big;
    }
    base::CopyBytes(staged.text.get(), path, path_bytes + 1);
    staged.used = path_bytes + 1;
    result      = stage_user_vector(frame.rsi, argc, staged, staged.arguments.get());
    if (result < 0) {
        return result;
    }
    if (argc == 0) {
        if (staged.used == staged.capacity) {
            return -kE2big;
        }
        staged.arguments[0]        = staged.text.get() + staged.used;
        staged.text[staged.used++] = '\0';
    }
    return stage_user_vector(frame.rdx, envc, staged, staged.environment.get());
}

long long map_entry_stack(const mm::AddressSpace& space, const loader::EntryStack& stack,
                          bool executable) {
    const auto kStart = base::math::AlignDown(stack.base, arch::page::kSize);
    const auto kEnd   = stack.base + stack.capacity;
    for (auto page = kStart; page < kEnd; page += arch::page::kSize) {
        const auto kFrame = mm::Pmm::self().allocate_page();
        if (kFrame == mm::Pmm::PhysAddr{}) {
            return -kEnomem;
        }
        auto* const kBytes = base::PtrAt<unsigned char>(mm::DirectMapVirt(kFrame.raw));
        base::SetBytes(kBytes, 0, arch::page::kSize);
        const auto kFrom  = base::math::Max(page, stack.base);
        const auto kUntil = base::math::Min(page + arch::page::kSize, kEnd);
        base::CopyBytes(kBytes + kFrom - page, stack.bytes + kFrom - stack.base, kUntil - kFrom);
        if (!space.map_user(page, kFrame.raw, arch::page::UserLeafTerms(true, executable))) {
            mm::Pmm::self().free_page(kFrame);
            return -kEnomem;
        }
    }
    return 0;
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
    const base::Result<fs::FsNode> kNode =
        kView.value().ops->lookup(kView.value().self, kView.value().rest);
    if (!kNode.ok()) {
        return -kEnoent;
    }
    fs::FsStat                 probed{};
    unsigned long              program_size = 0;
    base::Own<unsigned char[]> program;
    if (kView.value().ops->stat == nullptr ||
        !kView.value().ops->stat(kView.value().self, kNode.value(), probed).ok() ||
        !fs::ReadFilePrefix(kView.value(), 64_KiB, program, program_size).ok()) {
        return -kEio;
    }
    proc::Task* const              kSelf = CurrentTask();
    base::Shared<mm::AddressSpace> fresh(new mm::AddressSpace{});
    base::Shared<mm::VmaStore>     ranges(new mm::VmaStore{});
    if (!fresh.holds() || !ranges.holds() || !fresh->init()) {
        return -kEnomem;
    }
    EntryStaging staged;
    const auto   kStaged = stage_arguments(frame, path, staged);
    if (kStaged < 0) {
        return kStaged;
    }
    const unsigned long long kImageBias = image_load_bias(program.get(), program_size);
    const loader::LoadWorld  kWorld{
        .space         = *fresh,
        .vmas          = *ranges,
        .file          = kNode.value(),
        .file_bytes    = probed.size,
        .file_ops      = kView.value().ops,
        .file_self     = kView.value().self,
        .file_lifetime = kView.value().lifetime,
        .base          = kImageBias,
    };
    loader::EntryFacts facts{};
    if (!loader::LoadElfIntoSpace(program.get(), program_size, kWorld, facts)) {
        return -kEnomem;
    }
    facts.page_bytes             = cinux::arch::page::kSize;
    facts.platform               = loader::kPlatformName;
    unsigned int gnu_stack_flags = 0;
    const bool   kStackExecutable =
        loader::ReadGnuStackFlags(program.get(), program_size, gnu_stack_flags) &&
        (gnu_stack_flags & loader::kSegmentExec) != 0;
    constexpr unsigned long long kBrkSlideMask  = 0xFFF'000;
    constexpr unsigned long long kMmapSlideMask = 0x3FFF'FF000;
    ranges->seed_heap(cinux::base::math::AlignUp(ranges->max_end(), cinux::arch::page::kSize) +
                          (arch::RandomWord() & kBrkSlideMask),
                      mm::kUserBrkMmapSplit);
    ranges->set_mmap_ceiling(mm::kUserMmapTop - (arch::RandomWord() & kMmapSlideMask));
    char        interpreter_path[fs::kFsPathMax] = {};
    const char* interpreter                      = nullptr;
    if (loader::ReadInterpreterPath(program.get(), program_size, interpreter_path,
                                    sizeof(interpreter_path))) {
        if (facts.program_headers == 0) {
            return -kEnoexec;
        }
        interpreter = interpreter_path;
    }
    const auto kDyn = loader::LoadInterpreter(interpreter, *fresh, *ranges);
    if (!kDyn.ok()) {
        return -ErrnoFrom(kDyn.error());
    }
    facts.interpreter_base = kDyn.value().interpreter_base;
    const unsigned long long kFirstInstruction =
        kDyn.value().program_entry != 0 ? kDyn.value().program_entry : facts.program_entry;
    constexpr unsigned long long kStackSlideMask = 0x7FF'000;
    const unsigned long long     kStackTop =
        proc::kUserProgramStackTop - (arch::RandomWord() & kStackSlideMask);
    if (!proc::MapSignalTrampoline(*fresh, *ranges)) {
        return -kEnomem;
    }
    unsigned char entry_random[16] = {};
    facts.randomness               = draw_entry_random(entry_random);
    if (facts.randomness != nullptr) {
        facts.random_bytes = sizeof(entry_random);
    }
    facts.executable = staged.text.get();
    const loader::EntryRuns kRuns{
        .arguments   = staged.arguments.get(),
        .environment = staged.environment.get(),
    };
    const auto kPictureBytes = base::math::AlignUp(loader::EntryPictureBytes(kRuns, facts), 16UL);
    const auto kStackRegionFloor = proc::SignalTrampolineAddress() + arch::page::kSize;
    const auto kAvailableStack   = kStackTop - kStackRegionFloor - arch::page::kSize;
    if (kPictureBytes > kAvailableStack) {
        return -kE2big;
    }
    mm::VmaRegion stack_range{};
    const auto    kStackLimit = kSelf->limits[proc::kResourceStack][0];
    const auto    kStackSpan  = base::math::Max(
        base::math::AlignDown(
            base::math::Min(static_cast<unsigned long>(kStackLimit), kAvailableStack),
            arch::page::kSize),
        base::math::AlignUp(kPictureBytes, arch::page::kSize));
    stack_range.start      = kStackTop - kStackSpan - cinux::arch::page::kSize;
    stack_range.end        = kStackTop;
    stack_range.kind       = mm::VmaKind::kStack;
    stack_range.readable   = true;
    stack_range.writable   = true;
    stack_range.executable = kStackExecutable;
    if (!ranges->register_range(stack_range).ok()) {
        return -kEnomem;
    }
    const base::Own<unsigned char[]> kStackBytes(new unsigned char[kPictureBytes]{});
    if (!kStackBytes.holds()) {
        return -kEnomem;
    }
    const auto               kEntryRsp = kStackTop - kPictureBytes;
    const loader::EntryStack kStack{
        .bytes    = kStackBytes.get(),
        .base     = kEntryRsp,
        .capacity = kPictureBytes,
        .rsp      = kEntryRsp,
    };
    if (loader::WriteEntryPicture(kStack, kRuns, facts) == 0) {
        return -kE2big;
    }
    const auto kMapped = map_entry_stack(*fresh, kStack, kStackExecutable);
    if (kMapped < 0) {
        return kMapped;
    }
    kSelf->signals.reset();
    kSelf->clear_child_tid = 0;
    kSelf->files->close_on_exec();
    note_program_identity(*kSelf, path);
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
    auto& mutable_frame    = const_cast<SyscallFrame&>(frame);
    mutable_frame          = {};
    mutable_frame.user_rsp = kEntryRsp;
    mutable_frame.rcx      = kFirstInstruction;
    mutable_frame.r11      = kUserFlags;
    mutable_frame.rax      = 0;
    if (kSelf->vfork_done != nullptr) {
        auto* const kDone = kSelf->vfork_done;
        kSelf->vfork_done = nullptr;
        kDone->finish(true);
    }
    return 0;
}

}  // namespace cinux::syscall
