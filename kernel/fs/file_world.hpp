/**
 * @file    file_world.hpp
 * @brief   The kernel's one file-system world: a mount table, a root ramfs.
 *
 * Backends are instance things — a test world builds its own Vfs and
 * RamFs — but the running kernel wants exactly one of each, wired once
 * at bring-up: the ramfs mounted at the root, everything reachable
 * through resolve. That whole is the FileWorld singleton, after Pmm and
 * Tick. Later stations add backends by mounting them here; the world
 * itself never grows another face.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_fs
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <string_view>

#include "cinux/result.hpp"
#include "cinux/singleton.hpp"
#include "kernel/fs/ramfs/ramfs.hpp"
#include "kernel/fs/vfs.hpp"

namespace cinux::fs {

/**
 * @brief   The one mount table and its root backend, kernel-wide.
 * @note    Constant-initialized shell after Pmm and Tick: the mount
 *          table rides inside, while the ramfs — a tree that owns heap
 *          and therefore has a destructor — is planted by init() once
 *          the kernel heap is up, and lives for the rest of the boot.
 * @since   0.1.0
 * @ingroup kernel_fs
 */
class FileWorld : public cinux::base::Singleton<FileWorld> {
    friend class cinux::base::Singleton<FileWorld>;

public:
    /**
     * @brief         Plants the ramfs and mounts it at the root.
     *
     * @return        None
     * @note          Runs once at kernel bring-up; the mount refusing
     *                is a bring-up bug and dies loudly through Check.
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    void init();

    /**
     * @brief         Walks the baked-in archive into the ramfs.
     *
     * @return        None
     * @note          Runs inside init(), after the root mount. A tar
     *                that the kernel refuses to plant is a build bug
     *                and dies loudly through Check.
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    void plant_initrd();

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

private:
    FileWorld() = default;

    Vfs    vfs_;
    RamFs* ramfs_ = nullptr;
};

}  // namespace cinux::fs
