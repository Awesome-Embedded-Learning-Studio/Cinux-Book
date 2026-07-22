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

}  // namespace cinux::proc
