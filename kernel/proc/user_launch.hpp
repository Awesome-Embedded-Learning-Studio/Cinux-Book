/**
 * @file kernel/proc/user_launch.hpp
 * @brief Load a user program and jump to user mode (execve + user stack + jump)
 *
 * Shared "last mile" of user-process creation: after the caller has installed
 * a fresh AddressSpace (and, where relevant, an FDTable) on the current task,
 * launch_user_program() runs execve(), pre-maps the user stack pages, records
 * the demand-growth Stack VMA (F2-M5 hard gate), activates the address space,
 * and jumps to user mode.
 *
 * Consolidates the previously-duplicated launch sequence that lived inline in
 * kernel/proc/init.cpp (non-GUI shell fork) and kernel/gui/gui_init.cpp
 * (shell_child_entry). Never returns: on any failure the current task exits.
 *
 * Namespace: cinux::proc
 */

#pragma once

namespace cinux::proc {

/// Auxiliary-vector info gathered by execve() (full type in execve.hpp).
struct ElfAuxInfo;

/**
 * @brief execve() + user stack setup + jump to user mode (never returns)
 *
 * Preconditions on the current task:
 *   - addr_space is a freshly-constructed AddressSpace
 *   - (optional) fd_table wired up (e.g. terminal stdin/stdout pipes)
 *
 * @param path  program path to execve
 * @param argv  argument vector (nullptr-terminated)
 * @param envp  environment vector (nullptr-terminated)
 *
 * On execve failure, stack alloc failure, or VMA insert failure, exits the
 * current task. Otherwise jumps to user mode and never returns.
 */
void launch_user_program(const char* path, const char* const argv[], const char* const envp[]);

/**
 * @brief Finish a loaded program: lay the Linux initial stack and jump (no return)
 *
 * The "last mile" shared by launch_user_program() (fresh task) and the
 * sys_execve syscall handler (replace-current-image).  Assumes execve() has
 * already mapped the program's PT_LOAD segments into the current task's address
 * space and set ctx.rip = entry; @p aux carries the AT_PHDR/PHNUM/PHENT/ENTRY
 * gathered during load.  Pre-maps the stack pages, records the demand-growth
 * Stack VMA, builds argc/argv/envp/auxv into the top stack page, activates the
 * address space, and jumps to user mode.  Never returns.
 *
 * @param path  program path (also AT_EXECFN / argv[0])
 * @param argv  argument vector (nullptr-terminated)
 * @param envp  environment vector (nullptr-terminated)
 * @param aux   ELF auxv info from execve()
 */
void enter_loaded_program(const char* path, const char* const argv[], const char* const envp[],
                          const ElfAuxInfo& aux);

}  // namespace cinux::proc
