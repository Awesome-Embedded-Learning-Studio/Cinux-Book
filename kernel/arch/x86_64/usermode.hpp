/**
 * @file kernel/arch/x86_64/usermode.hpp
 * @brief User-mode (Ring 3) transition support
 *
 * Provides functions to initialise the SYSRET/SYSCALL infrastructure
 * and jump from kernel mode (Ring 0) to user mode (Ring 3).
 *
 * usermode_init() configures the STAR/EFER MSRs and allocates the
 * per-CPU GS data page used by syscall_entry for kernel stack access.
 *
 * Dependencies:
 *   - GDT must be initialised (user code/data selectors)
 *   - TSS must be loaded (RSP0 used on privilege level switches)
 *   - IDT must be set up (exceptions like #GP handled in Ring 0)
 *   - PMM must be initialised (for per-CPU GS page allocation)
 *   - AddressSpace must be initialised (for per-process page tables)
 *
 * Namespace: cinux::arch
 */

#pragma once

#include <stdint.h>

namespace cinux::arch {

// ============================================================
// User-mode constants
// ============================================================

/// Default virtual address for user program entry (linker base)
constexpr uint64_t USER_ENTRY_BASE = 0x400000;

/// Default virtual address for the top of the user stack
constexpr uint64_t USER_STACK_TOP = 0x7FFFFF000;

/// Number of 4 KB pages for the user stack (16 KB)
constexpr uint64_t USER_STACK_PAGES = 4;

/// Maximum user stack size -- the stack VMA spans this many bytes below
/// USER_STACK_TOP and grows down on demand (F2-M5). Only USER_STACK_PAGES are
/// pre-mapped at the top; the rest is demand-paged as the stack grows down.
/// Accesses below [USER_STACK_TOP - USER_STACK_GROWTH) hit no VMA -> segfault
/// (stack overflow guard).
constexpr uint64_t USER_STACK_GROWTH = 0x100000ULL;  // 1 MB

/// x86_64 SysV ABI: RSP = 8 mod 16 at _start entry (mimics `call` push)
constexpr uint64_t USER_ABI_RSP_OFFSET = 8;
static_assert((USER_STACK_TOP - USER_ABI_RSP_OFFSET) % 16 == 8,
              "User entry RSP must satisfy x86_64 ABI alignment");

// ============================================================
// User-mode functions
// ============================================================

/**
 * @brief Initialise the SYSCALL/SYSRET infrastructure
 *
 * Must be called once during boot, after GDT, IDT, and PMM are initialised.
 * Configures the STAR/EFER MSRs and allocates the per-CPU GS data page
 * used by syscall_entry for kernel stack access (gs:0).
 */
void usermode_init();

}  // namespace cinux::arch

// ============================================================
// Assembly entry points (C linkage)
// ============================================================

extern "C" {

/**
 * @brief Low-level Ring 0 -> Ring 3 transition via SYSRET
 *
 * This function does not return in the normal sense.  It performs
 * a SYSRET which loads RCX into RIP (user entry), R11 into RFLAGS
 * (with IF=1), and switches to Ring 3 CS/SS selectors.
 *
 * @param entry       Virtual address of the user program entry point
 * @param user_stack  Virtual address of the user stack top
 * @param arg         First argument to pass to the user program (in %rdi)
 */
void jump_to_usermode(uint64_t entry, uint64_t user_stack, uint64_t arg);

}  // extern "C"
