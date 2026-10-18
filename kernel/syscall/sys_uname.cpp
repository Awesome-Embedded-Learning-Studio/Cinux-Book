#include "kernel/syscall/sys_uname.hpp"

#include "cinux/memory.hpp"
#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

namespace {

/// One slot of the hosted record: 65 bytes, NUL included.
constexpr unsigned long kUtsFieldBytes = 65;

/// Six slots in order: sysname, nodename, release, version, machine,
/// domainname. The empty runs still land their NUL so a walking reader
/// never falls off the end.
void put_field(char* base, unsigned long slot, const char* text) {
    unsigned long index = 0;
    while (text[index] != '\0' && index < kUtsFieldBytes - 1) {
        ++index;
    }
    base::CopyBytes(base + (slot * kUtsFieldBytes), text, index);
    base[(static_cast<unsigned long long>(slot) * kUtsFieldBytes) + index] = '\0';
}

}  // namespace

long long HandleUname(const SyscallFrame& frame) {
    if (frame.rdi == 0 || frame.rdi >= kUserAddressLimit) {
        return -kEfault;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* const kSlots = reinterpret_cast<char*>(frame.rdi);
    base::SetBytes(kSlots, 0, 6 * kUtsFieldBytes);
    put_field(kSlots, 0, "Cinux");
    put_field(kSlots, 1, "cinux");
    put_field(kSlots, 2, "0.3.0");
    put_field(kSlots, 3, "");
    put_field(kSlots, 4, "x86_64");
    put_field(kSlots, 5, "");
    return 0;
}

}  // namespace cinux::syscall
