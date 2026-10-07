/**
 * @file    init_sequence.hpp
 * @brief   The bring-up ladder every subsystem climbs in order.
 *
 * Main used to name each bring-up call itself, and every new subsystem
 * made Main longer. The sequence is a fact about the machine, not a
 * fact about Main, so it lives here as data: an ordered table of steps,
 * each one receiving the boot info and handing back the boot info it
 * wants the next step to see. A step signals failure by returning
 * nullptr, and the runner stops the machine with the step's name on the
 * serial line — bring-up failure is a stop, not a limp. Order in the
 * table is order on the wire: serial before anything that prints, the
 * heap before anything that allocates, the interrupt controller before
 * the unmasking of lines.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_init
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::boot {
struct BootInfo;
}

namespace cinux::init {

/**
 * @brief         Signature every bring-up step shares.
 * @param[in]     info  The boot info as the previous step left it.
 * @return        The boot info the next step receives, or nullptr on
 *                failure. Most steps hand the pointer straight back.
 * @since         0.1.0
 * @ingroup       kernel_init
 */
using Step = const boot::BootInfo* (*)(const boot::BootInfo* info);

/**
 * @brief         One rung of the ladder: a name and the step under it.
 * @note          The name reaches the serial line on failure, so it is
 *                reader-facing spelling, not an identifier.
 * @since         0.1.0
 * @ingroup       kernel_init
 */
struct InitStep {
    const char* name;  ///< Serial-line label for this step.
    Step        run;   ///< The bring-up call itself.
};

/**
 * @brief         Climb the whole bring-up ladder.
 * @param[in,out] info  The boot info handed over from the entry path.
 * @return        The boot info as the last step left it, never nullptr
 *                — a failing step stops the machine instead.
 * @note          Each step runs to completion before the next; the
 *                first nullptr stops the climb with the step's name on
 *                the serial line and halts. Interrupts stay masked for
 *                the entire climb unless a step unmasks them.
 * @since         0.1.0
 * @ingroup       kernel_init
 */
[[nodiscard]] const boot::BootInfo* RunInitSequence(const boot::BootInfo* info);

}  // namespace cinux::init
