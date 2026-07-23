# kernel/fs -- VFS core (root) + per-FS backends (subdirs).
# Core: inode / file / vfs_mount / path (the generic VFS layer used by every
#       backend).  Backends: ext2/, procfs/, devfs/, ramdisk/ (one subdir per
#       on-disk / virtual filesystem, classified by nature -- not flat).

target_sources(big_kernel_common PRIVATE
    # --- VFS core ---
    ${CMAKE_CURRENT_SOURCE_DIR}/inode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/file.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vfs_mount.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/path.cpp
    # --- ramdisk ---
    ${CMAKE_CURRENT_SOURCE_DIR}/ramdisk/ramdisk.cpp
    # --- ext2 ---
    ${CMAKE_CURRENT_SOURCE_DIR}/ext2/ext2_common.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ext2/ext2_init.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ext2/ext2_inode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ext2/ext2_block.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ext2/ext2_directory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ext2/ext2_links.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ext2/ext2_metadata.cpp
    # --- devfs ---
    ${CMAKE_CURRENT_SOURCE_DIR}/devfs/devfs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/devfs/devfs_init.cpp
    # --- procfs ---
    ${CMAKE_CURRENT_SOURCE_DIR}/procfs/procfs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/procfs/procfs_content.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/procfs/procfs_init.cpp
)
