/**
 * @file    protections.hpp
 * @brief   The CR4 protection bits, asked of the CPU then opened.
 *
 * SMEP and SMAP belong to one processor at a time: writing either into
 * CR4 on a CPU that did not promise it fails the whole write with #GP,
 * so the enable asks CPUID first and only then touches the register.
 * The boot core opens both before the secondary cores arrive, and the
 * AP trampoline copies the boot core's CR4 image verbatim, so one
 * opening covers every core that comes online.
 *
 * @author  Charliechen114514
 * @date    2026-10-23
 * @version 0.3.0
 * @since   0.3.0
 * @ingroup kernel_arch
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::arch {

/**
 * @brief         Opens SMEP and SMAP on this core when the CPU promised
 *                them.
 *
 * @return        None.
 * @note          Reads CR4, ORs in each bit its CPUID answer named, and
 *                writes the word back — never rebuilds it, the machine
 *                owns the rest. A CPU without either bit simply keeps
 *                running without it; the mechanism printout tells the
 *                truth about what landed.
 * @since         0.3.0
 * @ingroup       kernel_arch
 */
void EnableCoreProtections();

}  // namespace cinux::arch
