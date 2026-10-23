#include "kernel/syscall/sys_uname.hpp"

#include <span>
#include <string_view>

#include "cinux/memory.hpp"
#include "cinux/text.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/uaccess.hpp"

namespace cinux::syscall {

namespace {

/// One slot of the hosted record: 65 bytes, NUL included.
constexpr unsigned long kUtsFieldBytes = 65;

/// Six slots in order: sysname, nodename, release, version, machine,
/// domainname. The empty runs still land their NUL so a walking reader
/// never falls off the end.
constexpr std::string_view kUtsFields[6] = {
    "Cinux", "cinux", "0.3.0", "", "x86_64", "",
};

}  // namespace

long long HandleUname(const SyscallFrame& frame) {
    if (frame.rdi == 0) {
        return -kEfault;
    }
    char slots[6 * kUtsFieldBytes];
    base::SetBytes(slots, 0, sizeof(slots));
    for (unsigned long slot = 0; slot < 6; ++slot) {
        const auto kField = std::span<char>(slots + (slot * kUtsFieldBytes),
                                            static_cast<unsigned long>(kUtsFieldBytes));
        base::CopyText(kField, kUtsFields[slot]);
    }
    if (!uaccess::CopyToUser(frame.rdi, slots, sizeof(slots))) {
        return -kEfault;
    }
    return 0;
}

}  // namespace cinux::syscall
