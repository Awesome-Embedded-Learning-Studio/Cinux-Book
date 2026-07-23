/**
 * @file kernel/proc/execve.hpp
 * @brief execve() result codes, errno aliases, and declaration (F3-M2 batch 5)
 *
 * Split out of process.hpp to keep process.hpp under the 500-line soft limit.
 * execve() is implemented in execve.cpp; called by sys_execve (kernel/syscall).
 *
 * Namespace: cinux::proc
 */

#pragma once

#include <cstdint>

namespace cinux::proc {

namespace errno_values {
constexpr int EPERM   = 1;   ///< Operation not permitted
constexpr int ENOENT  = 2;   ///< No such file or directory
constexpr int ESRCH   = 3;   ///< No such process
constexpr int EIO     = 5;   ///< I/O error
constexpr int ENOEXEC = 8;   ///< Exec format error
constexpr int ENOMEM  = 12;  ///< Out of memory
constexpr int EACCES  = 13;  ///< Permission denied
constexpr int EFAULT  = 14;  ///< Bad address
constexpr int ECHILD  = 10;  ///< No child processes
constexpr int EISDIR  = 21;  ///< Is a directory
constexpr int EINVAL  = 22;  ///< Invalid argument
}  // namespace errno_values

/**
 * @brief Result codes from execve() loading
 *
 * Values follow Linux errno conventions so that sys_execve can
 * return the negated value directly (e.g. -ENOENT, -ENOEXEC).
 */
enum class ExecveResult : int {
    Ok             = 0,    ///< Successfully loaded the new executable
    BadPath        = -22,  ///< Path is null or empty (EINVAL)
    FileNotFound   = -2,   ///< VFS could not resolve the path (ENOENT)
    FileNotRegular = -21,  ///< Path resolves to a non-regular file (EISDIR)
    ReadFailed     = -5,   ///< Failed to read the ELF data from the inode (EIO)
    BadElfMagic    = -8,   ///< ELF magic number mismatch (ENOEXEC)
    BadElfClass    = -8,   ///< Not a 64-bit ELF (ENOEXEC)
    BadElfEndian   = -8,   ///< Not little-endian (ENOEXEC)
    BadElfMachine  = -8,   ///< Not x86-64 (ENOEXEC)
    BadElfType     = -8,   ///< Not an executable (ENOEXEC)
    BadElfHeaders  = -8,   ///< Program header offset/size invalid (ENOEXEC)
    NoLoadSegments = -8,   ///< No PT_LOAD segments found (ENOEXEC)
    MapFailed      = -12,  ///< Address space map() failed for a segment (ENOMEM)
    NoAddressSpace = -12,  ///< Task has no address space (ENOMEM)
    NoCurrentTask  = -3,   ///< No current task in the scheduler (ESRCH)
};

/**
 * @brief ELF-derived auxiliary values the loader needs to build the entry stack
 *
 * execve() fills this when @p aux_out is non-null so the caller (e.g.
 * launch_user_program) can emit the AT_PHDR/AT_PHNUM/AT_PHENT/AT_ENTRY auxv
 * entries that musl/glibc read at startup to find the program headers.
 */
struct ElfAuxInfo {
    uint64_t at_phdr;     ///< User VA where the program headers are mapped
    uint64_t at_phnum;    ///< e_phnum (program header count)
    uint64_t at_phent;    ///< sizeof(Elf64_Phdr) = 56
    uint64_t at_entry;    ///< Main program entry (user VA); ldso jumps here after relocating
    uint64_t at_base;     ///< Interpreter (ldso) load base; 0 for a static executable
    bool     has_interp;  ///< True if a PT_INTERP was found and the interpreter loaded
};

/**
 * @brief Replace the current process image with a new ELF executable
 *
 * Reads the ELF binary from the VFS, validates the header, unmaps
 * existing user-space pages, loads PT_LOAD segments into the task's
 * address space, and sets the entry point.  The old process image is
 * destroyed but the PID, parent, and scheduler linkage are preserved.
 *
 * After a successful execve(), the caller is responsible for jumping
 * to the new entry point (typically via jump_to_usermode).
 *
 * @param path     Null-terminated path to the ELF executable
 * @param argv     Array of argument strings (may be nullptr)
 * @param envp     Array of environment strings (may be nullptr)
 * @param aux_out  If non-null, filled with ELF-derived AT_* values on success
 * @return ExecveResult::Ok on success, or an error code
 */
ExecveResult execve(const char* path, const char* const argv[], const char* const envp[],
                    ElfAuxInfo* aux_out = nullptr);

}  // namespace cinux::proc
