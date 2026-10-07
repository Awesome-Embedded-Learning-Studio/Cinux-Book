/**
 * @file    trace.hpp
 * @brief   The kernel's stack tracer: walk the frame chain, print it.
 *
 * One entry point, one input: a frame base to start from. The walk
 * follows the rbp chain the frame-pointer flag keeps alive on every
 * compiled function — slot 0 of a frame holds the next frame base,
 * slot 1 the address to return to — and prints each return address as
 * one line of the dump. Frame-pointer discipline is what makes this
 * possible at all, which is why the kernel builds with
 * -fno-omit-frame-pointer; hand-written assembly that skips the chain
 * (context_switch, boot stubs) ends a walk early rather than corrupting
 * it, because every link is validated before it is followed. Addresses
 * print raw: turning them into names is an offline job for addr2line,
 * which keeps a symbol table out of the shipped kernel until the
 * station that actually needs one.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_debug
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::debug {

/**
 * @brief         Walk the frame chain from one frame base and print it.
 * @param[in]     frame_base   rbp value of the frame to start from.
 * @return        None.
 * @note          The walk validates each link (non-null, 16-byte
 *                aligned, kernel half) before following it, and stops
 *                at the first link that fails or after
 *                kMaxTraceFrames frames. Runs on the crash path, so
 *                it depends on nothing beyond print and the layout
 *                facts.
 * @since         0.1.0
 * @ingroup       kernel_debug
 */
void TraceFrom(unsigned long long frame_base);

}  // namespace cinux::debug
