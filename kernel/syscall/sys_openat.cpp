#include "kernel/syscall/sys_openat.hpp"

#include <cstdint>

#include "kernel/syscall/sys_open.hpp"
#include "kernel/syscall/sys_stat.hpp"
#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

namespace {

/// The hosted convention for "relative to where I stand": the working
/// directory itself, spelled as a sentinel descriptor.
constexpr std::int32_t kAtFdcwd = -100;

}  // namespace

long long HandleOpenat(const SyscallFrame& frame) {
    const auto kNamed = static_cast<std::int32_t>(frame.rdi);
    if (kNamed != kAtFdcwd) {
        return -kEnotdir;
    }
    SyscallFrame flat{};
    flat.rdi = frame.rsi;
    flat.rsi = frame.rdx;
    return HandleOpen(flat);
}

long long HandleNewfstatat(const SyscallFrame& frame) {
    const auto kNamed = static_cast<std::int32_t>(frame.rdi);
    if (kNamed != kAtFdcwd) {
        return -kEnotdir;
    }
    SyscallFrame flat{};
    flat.rdi = frame.rsi;
    flat.rsi = frame.rdx;
    return HandleStat(flat);
}

}  // namespace cinux::syscall
