/**
 * @file kernel/arch/x86_64/tls.cpp
 * @brief TLS base register helpers (F3-M2 batch 1)
 *
 * Thin wrmsr/rdmsr wrappers over MSR_FS_BASE backing set/get_tls_base().
 */

#include "kernel/arch/x86_64/tls.hpp"

namespace cinux::arch {

void set_tls_base(uint64_t addr) {
    uint32_t low  = static_cast<uint32_t>(addr & 0xFFFFFFFFu);
    uint32_t high = static_cast<uint32_t>(addr >> 32);
    __asm__ volatile("wrmsr" : : "c"(kMsrFsBase), "a"(low), "d"(high));
}

uint64_t get_tls_base() {
    uint32_t low  = 0;
    uint32_t high = 0;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(kMsrFsBase));
    return (static_cast<uint64_t>(high) << 32) | static_cast<uint64_t>(low);
}

}  // namespace cinux::arch
