#include "kernel/syscall/sys_capability.hpp"

#include <cstdint>

#include "kernel/proc/scheduler.hpp"
#include "kernel/syscall/syscall_internal.hpp"
#include "kernel/syscall/uaccess.hpp"

namespace cinux::syscall {
namespace {
constexpr std::uint32_t kVersion1 = 0x19980330;
constexpr std::uint32_t kVersion2 = 0x20071026;
constexpr std::uint32_t kVersion3 = 0x20080522;
struct Header { std::uint32_t version; std::int32_t pid; };
struct Data { std::uint32_t effective; std::uint32_t permitted; std::uint32_t inheritable; };
long long read_header(Word address, Header& header, unsigned long& bytes) {
    if (!uaccess::CopyFromUser(&header, address, sizeof(header))) {
        return -kEfault;
    }
    if (header.version != kVersion1 && header.version != kVersion2 && header.version != kVersion3) {
        return uaccess::CopyToUser(address, &kVersion3, sizeof(kVersion3)) ? -kEinval : -kEfault;
    }
    bytes = (header.version == kVersion1 ? 1UL : 2UL) * sizeof(Data);
    return 0;
}
}

long long HandleCapget(const SyscallFrame& frame) {
    Header header{};
    unsigned long bytes = 0;
    const auto error = read_header(frame.rdi, header, bytes);
    if (error != 0 || frame.rsi == 0) { return error; }
    if (header.pid < 0) { return -kEinval; }
    if (header.pid != 0 && static_cast<unsigned long long>(header.pid) != CurrentTask()->pid) {
        bool found = false;
        proc::Scheduler::self().with_task(static_cast<unsigned long long>(header.pid),
                                          [&](const proc::Task&) { found = true; });
        if (!found) { return -kEsrch; }
    }
    // Cinux does not grant file capabilities. Traditional uid checks remain
    // independent; an ordinary process can only hold the empty capability set.
    const Data empty[2]{};
    return uaccess::CopyToUser(frame.rsi, empty, bytes) ? 0 : -kEfault;
}

long long HandleCapset(const SyscallFrame& frame) {
    Header header{};
    unsigned long bytes = 0;
    const auto error = read_header(frame.rdi, header, bytes);
    if (error != 0) { return error; }
    if (header.pid != 0 && static_cast<unsigned long long>(header.pid) != CurrentTask()->pid) {
        return -kEperm;
    }
    Data wanted[2]{};
    if (!uaccess::CopyFromUser(wanted, frame.rsi, bytes)) { return -kEfault; }
    for (const auto& word : wanted) {
        if (word.effective != 0 || word.permitted != 0 || word.inheritable != 0) {
            return -kEperm;
        }
    }
    return 0;
}
}
