/**
 * @file    idt.hpp
 * @brief   The kernel's full 256-entry interrupt descriptor table.
 *
 * The table lives in .bss at zero file cost and is filled by whoever owns
 * a vector range — exception stubs first, IRQ stubs when the PIC wakes up.
 * Gate encoding is a pure constexpr function so the host world can pin
 * every field split in test_idt before any of it reaches hardware.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_arch
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::arch::idt {

/// Present, DPL 0, 32-bit interrupt gate: the one gate type this kernel arms.
inline constexpr unsigned char kTypeInterruptGate = 0x8E;

struct [[gnu::packed]] GateEntry {
    unsigned short offset_low;
    unsigned short selector;
    unsigned char  ist;
    unsigned char  type_attr;
    unsigned short offset_mid;
    unsigned int   offset_high;
    unsigned int   reserved;
};

/**
 * @brief         Split one handler address into a gate entry.
 * @param[in]     handler    Linear address of the stub the gate points at.
 * @param[in]     selector   Code selector the CPU switches to on entry.
 * @param[in]     ist        Interrupt-stack-table slot, 0 for none.
 * @param[in]     type_attr  Gate type byte, kTypeInterruptGate for handlers.
 * @return        The encoded 16-byte gate.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
constexpr GateEntry EncodeGate(unsigned long long handler, unsigned short selector,
                               unsigned char ist, unsigned char type_attr) {
    return GateEntry{.offset_low  = static_cast<unsigned short>(handler & 0xFFFF),
                     .selector    = selector,
                     .ist         = ist,
                     .type_attr   = type_attr,
                     .offset_mid  = static_cast<unsigned short>((handler >> 16) & 0xFFFF),
                     .offset_high = static_cast<unsigned int>(handler >> 32),
                     .reserved    = 0};
}

struct [[gnu::packed]] TablePointer {
    unsigned short     limit;
    unsigned long long base;
};

/**
 * @brief         Write one gate into the table.
 * @param[in]     vector   Vector number, 0..255.
 * @param[in]     entry    Encoded gate value.
 * @return        None.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
void InstallGate(unsigned int vector, GateEntry entry);

/**
 * @brief         Point the CPU at the table with one lidt.
 * @return        None.
 * @note          Nothing fires through gates that were never installed —
 *                an absent gate raises #GP with the vector in its error
 *                code, which the exception dump then names.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
void LoadIdt();

}  // namespace cinux::arch::idt
