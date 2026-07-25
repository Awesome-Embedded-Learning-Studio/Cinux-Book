---
title: 06 · 挂载表 MountPoint + owned 生命周期
---

# 挂载表 MountPoint + owned 生命周期

## 挂载表 MountPoint + owned 生命周期(承 017)

owned 这个 bool 的故事,017 tmpfs 章讲过——那里看的是 `owned=true` 在 tmpfs 通路怎么用(堆 `new TmpFs` → `release()` → `vfs_mount_add(owned=true)` → `umount2` 走 `free_tree` 回收整棵树)。这里看它在挂载表**全貌怎么收敛三类对象**。先看结构:

```cpp
struct MountPoint {
    char        path[MOUNT_PATH_MAX];  ///< Absolute path prefix (e.g. "/")
    FileSystem* fs;                    ///< Concrete filesystem backend
    bool        in_use;                ///< Whether this slot is occupied
    /// True when @p fs was heap-allocated by sys_mount and must therefore be
    /// `delete`d by vfs_mount_remove (sys_umount2).  Boot/static mounts wire a
    /// static FileSystem (g_devfs, g_procfs, g_tmpfs, ...) and leave this false,
    /// so removing them frees nothing.  Default false keeps every existing
    /// caller (boot wiring + tests with stack-local/mock backends) unchanged.
    bool        owned{false};
};
```

(`vfs_mount.hpp:38-48`。)挂载表是定长数组 `MountPoint g_mount_table[MOUNT_TABLE_SIZE]`(`MOUNT_TABLE_SIZE = 8`,`vfs_mount.hpp:26`),一把 `Spinlock` 串行化(`vfs_mount.cpp:26`)。每个槽四个字段:路径前缀、FS 指针、是否占用、**是否 owned**。

三类对象在一个 bool 上收敛:

**第一类:boot/static 接线,`owned=false`。** boot 时挂的根 FS(`/`，`init.cpp:123` 的 `vfs_mount_add("/", rootfs)`)、tmpfs(`/tmp`，`tmpfs_init.cpp:44`)、devfs(`/dev`，`devfs_init.cpp:174`)、procfs(`/proc`，`procfs_init.cpp:39`)——这四个对象是 `.bss` 静态全局或 boot 栈局部,命超表(整个内核运行期)。`vfs_mount_add` 默认 2-arg(`owned = false`,`vfs_mount.hpp:79`),挂载表只存指针、不管释放。`umount2` 看到这类只摘槽、不 delete。

**第二类:`sys_mount` 堆分配,`owned=true`。** tmpfs 分支(`sys_mount.cpp:61`)和 ext2/ext4 分支(`sys_mount.cpp:115`)——`new` 一个后端 FS,`mount()` 成功后 `release()` 交裸指针给挂载表,`owned=true`。`umount2` 看到 owned 就 `delete fs`,tmpfs 的 `delete` 触发 `free_tree` 递归回收整棵树,ext2 的 `delete` 触发 `~Ext2` 释放缓存对象。

**第三类:`sys_mount -t proc/devfs` 二次挂载单例,`owned=false`。** `sys_mount.cpp:79` 那条腿——用户运行时 `mount -t proc none /mnt/proc2`,挂载表登记一个新槽指向 boot 单例,`owned=false`。`umount2 /mnt/proc2` 只摘新挂点,**原 `/proc` 不动**——单例的命跟 boot 接线一样长,二次挂载只是给它多一个路径前缀。

这三类的回收分叉全在 `vfs_mount_remove` 里:

```cpp
bool vfs_mount_remove(const char* path) {
    if (path == nullptr) return false;
    auto g = g_mount_lock.guard();
    for (uint32_t i = 0; i < MOUNT_TABLE_SIZE; ++i) {
        if (g_mount_table[i].in_use && strncmp(g_mount_table[i].path, path, MOUNT_PATH_MAX) == 0) {
            // Ownership-aware teardown: a sys_mount-created backend (owned=true)
            // is heap-allocated and has no other owner, so delete it here.  A
            // boot/static mount (owned=false) wires a static object and is left
            // alone -- its lifetime exceeds the table.  The TmpFs / ProcFs / DevFs
            // destructors never touch g_mount_lock, so this is safe under it.
            if (g_mount_table[i].owned) {
                delete g_mount_table[i].fs;
            }
            g_mount_table[i].in_use = false;
            g_mount_table[i].fs     = nullptr;
            g_mount_table[i].owned  = false;
            return true;
        }
    }
    return false;
}
```

(`vfs_mount.cpp:79-104`。)一个 `if (owned) delete fs` 就是全部分叉——`owned=true` 先 `delete fs` 再清槽,`owned=false` 只摘点。这一节短,核心就是「017 讲过的 owned,这里看它在挂载表全貌收敛三类」——tmpfs 通路的 RAII 细节(`unique_ptr` 守到 `mount()` 成功才 `release`、错误腿显式 `delete`)回 017 看,这里不重讲。

> **新手会本能想给 `FileSystem` 加 `virtual Destroy()` / `shared_ptr` / deleter / `MountKind` 枚举。** Cinux 的解法极朴素:结构体里多一个 bool 默认 false。boot 接线和 `sys_mount` 走**同一个 `vfs_mount_add`**,差别只在第三个实参。这是「用默认参兼容旧调用者」的典型范例——加字段不破坏任何既有 2-arg 调用(所有 boot 接线 + 测试栈/mock FS 都不用改一行)。一个坑:若误把 boot 静态挂载标 `owned=true`,`umount` 时 `delete` 静态对象 → 双重释放/崩溃。`test_remount_after_umount_is_fresh`(`test_mount.cpp:104-125`)是 owned 语义最硬的证据——`umount` 后再 `mount` 同路径,文件不见了,证明 owned 后端真被 `delete` 了,新 `mount` 是全新实例不是 stale 残留。
