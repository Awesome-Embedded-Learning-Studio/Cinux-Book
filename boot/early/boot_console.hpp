/**
 * @file    boot_console.hpp
 * @brief   Character output primitives for the early boot path.
 *
 * Every byte lands on the QEMU debug console: no hardware state is
 * touched, which keeps these calls safe from real mode before anything
 * else on the machine is initialised.
 *
 * @author  Charliechen114514
 * @version 0.1
 * @date    2026-09-27
 * @since   0.1.0
 * @ingroup boot_early
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "boot_port.hpp"

namespace cinux::boot::serial {

/**
 * @brief Send one character to the debug console.
 *
 * @param[in] character Byte to emit.
 */
inline void PutChar(char character) {
    asm volatile("outb %0, %1" : : "a"(character), "Nd"(kDebugconPort));
}

inline void PutString(char const* a_string) {
    while (*a_string != '\0') {
        PutChar(*a_string);
        ++a_string;
    }
}

}  // namespace cinux::boot::serial