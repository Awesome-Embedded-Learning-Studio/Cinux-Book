/**
 * @file kernel/arch/x86_64/backtrace.hpp
 * @brief Frame-pointer-based kernel stack backtrace (x86-64)
 *
 * Walks the RBP chain to collect call-stack return addresses, then symbolizes
 * them via KALLSYMS.  Aligns with Linux's dump_stack()/CONFIG_FRAME_POINTER:
 * because every function keeps a frame pointer (-fno-omit-frame-pointer, batch
 * 0), the saved-RBP slots form a singly-linked list of stack frames.
 *
 * Safety (the whole point for the panic path): every frame pointer is validated
 * with VMM::translate() before it is dereferenced, the depth is capped, and a
 * non-advancing or null chain stops the walk.  A kernel #PF would halt, so we
 * must never follow an unmapped frame pointer -- a corrupt stack yields a
 * short trace, not a crash.
 *
 * Namespace: cinux::arch
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cinux::arch {

/// Hard cap on captured frames; defends against a corrupt/circular chain.
constexpr size_t kBacktraceMaxFrames = 64;

/**
 * @brief Walk the frame-pointer chain, collecting return addresses.
 *
 * Starts at @p rbp (a saved frame pointer) and writes each frame's return
 * address into @p addrs until @p max is reached, the chain ends (next RBP is
 * 0), a page is unmapped, or the chain stops advancing upward.  Safe to call
 * at IF=0 (no allocation; uses VMM::translate which is lock-tolerant).
 *
 * @param rbp    Starting frame pointer (saved RBP of the first frame).
 * @param addrs  Output array for return addresses.
 * @param max    Capacity of @p addrs.
 * @return Number of frames written.
 */
size_t backtrace_capture(uint64_t rbp, uint64_t* addrs, size_t max);

/**
 * @brief Capture and symbolize-print up to @p max_frames frames from @p rbp.
 *
 * @param rbp          Starting frame pointer.
 * @param max_frames   Cap (0 = kBacktraceMaxFrames).
 */
void backtrace_from(uint64_t rbp, size_t max_frames = 0);

/**
 * @brief Backtrace from the caller's frame.
 *
 * Reads the current RBP and dumps the call stack.  The first frame shown is
 * the caller of backtrace() itself.
 */
void backtrace();

}  // namespace cinux::arch
