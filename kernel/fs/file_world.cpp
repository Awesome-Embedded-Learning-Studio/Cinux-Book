/**
 * @file    file_world.cpp
 * @brief   Wiring the world: one ramfs, one root, one baked-in archive.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_fs
 * @copyright Copyright (c) 2026
 */

#include "kernel/fs/file_world.hpp"

#include <string_view>

#include "cinux/assert.hpp"
#include "cinux/result.hpp"
#include "kernel/fs/ramfs/ramfs.hpp"
#include "kernel/fs/ustar.hpp"
#include "kernel/fs/vfs.hpp"

extern "C" {

/// First byte of the initrd archive the linker baked into the image.
extern unsigned char g_initrd_start[];
/// One past the last byte of that archive.

extern unsigned char g_initrd_end[];
}  // extern "C"

namespace cinux::fs {

void FileWorld::init() {
    ramfs_                            = new RamFs{};
    const base::Result<void> kMounted = vfs_.mount("/", *ramfs_);
    base::safety::Check(kMounted.ok(), "root ramfs mount failed");
    plant_initrd();
}

void FileWorld::plant_initrd() {
    const unsigned long kArchiveBytes = reinterpret_cast<unsigned long>(g_initrd_end) -
                                        reinterpret_cast<unsigned long>(g_initrd_start);
    UstarReader         reader(g_initrd_start, kArchiveBytes);
    UstarEntry          entry;
    while (reader.next(entry).value()) {
        if (entry.type == InodeType::kDirectory) {
            std::string_view path = entry.name;
            if (path.ends_with('/')) {
                path.remove_suffix(1);
            }
            const base::Result<FsNode> kDir = ramfs_->make_dir(path);
            base::safety::Check(kDir.ok(), "initrd directory refused");
            continue;
        }
        const base::Result<FsNode> kFile = ramfs_->make_file(entry.name);
        base::safety::Check(kFile.ok(), "initrd file refused");
        const base::Result<unsigned long> kStored =
            ramfs_->write(kFile.value(), 0, entry.data.data(), entry.size);
        base::safety::Check(kStored.ok(), "initrd content refused");
    }
}

base::Result<FsView> FileWorld::resolve(std::string_view path) {
    return vfs_.resolve(path);
}

}  // namespace cinux::fs
