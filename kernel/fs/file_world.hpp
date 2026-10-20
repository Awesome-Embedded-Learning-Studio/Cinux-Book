/**
 * @file    file_world.hpp
 * @brief   The kernel's one file-system world: a mount table, an ext2 root.
 *
 * Backends are instance things — a test world builds its own Vfs and
 * mocks — but the running kernel wants exactly one of each, wired once
 * at bring-up: the block queue over the AHCI drive, the ext2
 * volume over the queue, mounted at the root. The queue sleeps on its
 * requests, so init() runs inside the kernel_init task — the named
 * first thread of the system, PID 1 in the borrowed Linux vocabulary
 * — never on the boot stack. The whole is the FileWorld singleton,
 * after Pmm and Tick. The first backend, ramfs, stays in the tree as
 * the host tests' volume and the concept's first example; the
 * production root is the disk.
 *
 * @author  Charliechen114514
 * @date    2026-10-11
 * @version 0.3
 * @since   0.1.0
 * @ingroup kernel_fs
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <string_view>

#include "cinux/result.hpp"
#include "cinux/singleton.hpp"
#include "kernel/fs/devfs/devfs.hpp"
#include "kernel/fs/procfs/procfs.hpp"
#include "kernel/fs/vfs.hpp"

namespace cinux::driver {
class BlockCache;
class BlockQueue;
}  // namespace cinux::driver

namespace cinux::fs::ext2 {
class Ext2;
}  // namespace cinux::fs::ext2

namespace cinux::fs {

/**
 * @brief   The one mount table and its disk-backed root, kernel-wide.
 * @note    Constant-initialized shell after Pmm and Tick: the mount
 *          table rides inside, while the queue and the volume —
 *          objects that own state and therefore want the heap — are
 *          planted by init() once block devices are up, and live for
 *          the rest of the boot.
 * @since   0.1.0
 * @ingroup kernel_fs
 */
struct FileTable;

class FileWorld : public cinux::base::Singleton<FileWorld> {
    friend class cinux::base::Singleton<FileWorld>;

public:
    /**
     * @brief   Plants the queue and volume, mounts ext2 at the root and
     *          the device table at /dev.
     *
     * @return  None
     * @note    Runs exactly once, from inside the kernel_init task:
     *          every block ask sleeps on the queue, so the caller must
     *          already be schedulable. A volume refusing to mount is
     *          a build or wiring bug and dies loudly. DevFs carries no
     *          I/O of its own, so it rides as a plain member and its
     *          seat at /dev costs no queue.
     * @since   0.1.0
     * @ingroup kernel_fs
     */
    void init();

    /**
     * @brief         Maps an absolute path onto the deepest mount.
     *
     * @param[in]     path   Absolute path to resolve.
     * @return        The winning backend plus the mount-relative rest,
     *                or the resolve errors.
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<FsView> resolve(std::string_view path);

    /**
     * @brief         Seats a table's standard streams on the console.
     *
     * @param[in,out] table   The table receiving seats zero through two.
     * @return        Success, or the resolve, lookup or seating errors.
     * @note          One console open, shared by all three seats, read-
     *                write from birth — the shape a shell expects to
     *                inherit, and the shape that makes the descriptor
     *                numbers' fixed meanings real: closing seat zero
     *                really closes it, and the next open may take it.
     * @since         0.5.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<void> seat_console_streams(FileTable& table);

private:
    FileWorld() = default;

    Vfs    vfs_;
    DevFs  devfs_;
    ProcFs procfs_;

    driver::BlockQueue* queue_  = nullptr;
    driver::BlockCache* cache_  = nullptr;
    ext2::Ext2*         volume_ = nullptr;
};

}  // namespace cinux::fs
