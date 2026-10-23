#include "kernel/syscall/sys_getrandom.hpp"

#include "kernel/arch/x86_64/rdrand.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/uaccess.hpp"
#include "kernel/time/tick.hpp"

namespace cinux::syscall {

namespace {

/// One xorshift round over the state word.
unsigned long long scramble(unsigned long long state) {
    state ^= state << 13U;
    state ^= state >> 7U;
    state ^= state << 17U;
    return state;
}

}  // namespace

long long HandleGetrandom(const SyscallFrame& frame) {
    if (frame.rdi == 0) {
        return -kEfault;
    }
    const unsigned long long kWant = frame.rsi;
    if (kWant > 256) {
        return -kEinval;
    }
    unsigned long long state = 0;
    if (!cinux::arch::TryRdrand(state)) {
        state = time::Tick::self().since_boot() ^ (frame.rdi << 17U) ^ 0x9E3779B97F4A7C15ULL;
    }
    unsigned char out[256];
    for (unsigned long long index = 0; index < kWant; ++index) {
        state      = scramble(state);
        out[index] = static_cast<unsigned char>(state >> 32U);
    }
    if (!uaccess::CopyToUser(frame.rdi, out, kWant)) {
        return -kEfault;
    }
    return static_cast<long long>(kWant);
}

}  // namespace cinux::syscall
