/**
 * @file    procfs.hpp
 * @brief   The process file system: living facts behind read-only names.
 *
 * Every other backend answers with bytes some medium holds. This one
 * answers with facts the kernel knows right now: the memory counters,
 * the task roll, each task's vital signs. Nothing is stored — a lookup
 * mints a node for a name, a read generates the bytes on the spot, and
 * the next read may honestly differ because the facts did. The class
 * knows nothing about tasks or memory itself: every fact arrives
 * through the injected source, the same seam devfs gives its devices,
 * so a host test drives the whole tree with fakes.
 *
 * @author  Charliechen114514
 * @date    2026-10-19
 * @version 0.1
 * @since   0.5.0
 * @ingroup kernel_fs
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <string_view>

#include "cinux/result.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/proc/task_snapshot.hpp"

namespace cinux::fs {

/// Where the per-pid inode numbers begin, above the fixed names.
inline constexpr unsigned long kProcPidBase = 0x100;

/// The self/exe node's own inode number.
inline constexpr unsigned long kProcExeIno = 4;

/**
 * @brief   The facts one procfs instance answers with.
 * @note    Every seat is a function pointer over a context the
 *          assembler owns; the kernel wires the scheduler, the page
 *          manager and the current task, a test wires fakes. Null
 *          seats answer as empty facts — an unread source is not an
 *          error, it is a quiet file.
 * @since   0.5.0
 * @ingroup kernel_fs
 */
struct ProcSource {
    /// Names the ordinal-th living process leader; false past the last.
    bool (*nth_tgid)(void* context, unsigned long ordinal, unsigned long& tgid) = nullptr;

    /// Copies one task's vital signs; false for no such task.
    bool (*snapshot)(void* context, unsigned long tgid, proc::TaskSnapshot& into) = nullptr;

    /// Copies the current task's program path; returns its length.
    unsigned long (*exe_path)(void* context, char* out, unsigned long cap) = nullptr;

    /// Reads the current task's program bytes; false past the end.
    bool (*exe_bytes)(void* context, unsigned long offset, void* sink, unsigned long count,
                      unsigned long& gave) = nullptr;

    /// The current task's program size in bytes.
    unsigned long (*exe_size)(void* context) = nullptr;

    /// The memory counters, in pages.
    void (*memory)(void* context, unsigned long& free_pages, unsigned long& total_pages) = nullptr;

    /// The word every seat receives back.
    void* context = nullptr;
};

/**
 * @brief   Read-only directory of living facts, generated on demand.
 * @note    The root holds the fixed names — meminfo, self — beside one
 *          directory per living process leader; each process directory
 *          holds stat and cmdline. The lookups gate on life: a dead
 *          pid answers NotFound, never stale facts. self/exe is the
 *          one lie worth telling exactly: it stands for the current
 *          task's program, and its bytes come from the source's
 *          delegate so exec itself can read through it.
 * @since   0.5.0
 * @ingroup kernel_fs
 */
class ProcFs {
public:
    ProcFs() = default;

    ProcFs(const ProcFs&)            = delete;
    ProcFs& operator=(const ProcFs&) = delete;

    /**
     * @brief         Installs the facts this instance answers with.
     *
     * @param[in]     source   The facts and their context.
     * @return        None
     * @note          Bring-up wiring, single-threaded, before the mount
     *                goes live. The source is borrowed for the
     *                instance's whole life.
     * @since         0.5.0
     * @ingroup       kernel_fs
     */
    void set_source(const ProcSource& source);

    [[nodiscard]] base::Result<FsNode>        lookup(std::string_view path);
    [[nodiscard]] base::Result<FsNode>        make_file(std::string_view path);
    [[nodiscard]] base::Result<FsNode>        make_dir(std::string_view path);
    [[nodiscard]] base::Result<void>          unlink(std::string_view path);
    [[nodiscard]] base::Result<unsigned long> read(FsNode node, unsigned long offset, void* sink,
                                                   unsigned long count);
    [[nodiscard]] base::Result<unsigned long> write(FsNode node, unsigned long offset,
                                                    const void* source, unsigned long count);
    [[nodiscard]] base::Result<bool>          read_dir(FsNode node, unsigned long index,
                                                       FsDirent& entry) const;
    [[nodiscard]] base::Result<void>          stat(FsNode node, FsStat& probed);
    /**
     * @brief         Pins a node for an open descriptor.
     *
     * @param[in]     node   Handle from lookup.
     * @return        None
     * @note          No-op: facts have no life to hold.
     * @since         0.5.0
     * @ingroup       kernel_fs
     */
    void                                      retain(FsNode node);

    /**
     * @brief         Drops an open descriptor's pin on a node.
     *
     * @param[in]     node   Handle from lookup.
     * @return        None
     * @note          No-op: facts have no life to hold.
     * @since         0.5.0
     * @ingroup       kernel_fs
     */
    void               release(FsNode node);
    [[nodiscard]] bool readable(FsNode node);
    /**
     * @brief         Asks one device question.
     *
     * @param[in]     node       Any handle.
     * @param[in]     word       The question word.
     * @param[in]     argument   The question's argument word.
     * @return        -ENOTTY: no fact is a device.
     * @since         0.5.0
     * @ingroup       kernel_fs
     */
    long long          ioctl(FsNode node, unsigned long long word, unsigned long long argument);
    [[nodiscard]] base::Result<FsNode> adopt_open(FsNode node, std::string_view path);

private:
    ProcSource source_{};
};

}  // namespace cinux::fs
