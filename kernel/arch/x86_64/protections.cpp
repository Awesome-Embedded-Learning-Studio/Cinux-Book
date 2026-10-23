/**
 * @file    protections.cpp
 * @brief   The opening itself, on the core that calls.
 *
 * @author  Charliechen114514
 * @date    2026-10-23
 * @version 0.3.0
 * @since   0.3.0
 * @ingroup kernel_arch
 * @copyright Copyright (c) 2026
 */

#include "kernel/arch/x86_64/protections.hpp"

#include "kernel/arch/x86_64/cpuid.hpp"
#include "kernel/arch/x86_64/registers.hpp"

namespace cinux::arch {

void EnableCoreProtections() {
    unsigned long long cr4 = ReadCr4();
    if (HasSmep()) {
        cr4 |= kCr4Smep;
    }
    if (HasSmap()) {
        cr4 |= kCr4Smap;
    }
    WriteCr4(cr4);
}

}  // namespace cinux::arch
