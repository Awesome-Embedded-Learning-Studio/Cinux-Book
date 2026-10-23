/**
 * @file    entropy.hpp
 * @brief   The kernel's one word source for layout jitter.
 *
 * Seeds the shared generator from what this machine alone can say:
 * hardware draws when the CPU offers them, the cycle counter, and the
 * kernel's own landing address. None of these alone is secret; mixed
 * together they answer the question the layout consumers ask — that
 * no two boots agree on where things land. The generator itself is
 * the base tree's plain xoshiro; callers with adversaries bring their
 * own hardware draws.
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
 * @brief         Seeds the shared generator from this machine's own
 *                facts.
 *
 * @return        None.
 * @note          Runs once, early in the boot; before it runs, draws
 *                answer zero and every layout stays at its compiled-in
 *                seat — degraded but honest, never a trap.
 * @since         0.3.0
 * @ingroup       kernel_arch
 */
void SeedKernelRandom();

/**
 * @brief         Draws one word from the shared generator.
 *
 * @return        The next pseudorandom word; zero before the seed ran.
 * @since         0.3.0
 * @ingroup       kernel_arch
 */
[[nodiscard]] unsigned long long RandomWord();

}  // namespace cinux::arch
