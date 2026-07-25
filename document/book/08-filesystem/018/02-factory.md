---
title: 02 · mount factory:四类 fstype 怎么分发
---

# mount factory:四类 fstype 怎么分发

## mount factory:四类 fstype 怎么分发

把 `do_mount_kernel` 当一个**工厂方法**看:输入三个字符串(`source`/`target`/`fstype`)+ 一个 `flags`,输出一个挂在挂载表里的 `FileSystem`,或者一个 errno。先把签名和入口校验摆出来:

```cpp
int64_t do_mount_kernel(const char* source, const char* target, const char* fstype, uint64_t flags) {
    // MS_* flags are accepted for Linux ABI parity but not yet modelled.
    if (target == nullptr || fstype == nullptr || target[0] == '\0') {
        return -kEinval;
    }
    // ...
}
```

(`sys_mount.cpp:45-49`。)`target` 和 `fstype` 是必填(没 target 挂哪、没 fstype 不知道造什么 FS),`source` 只对 ext2/ext4 必填(tmpfs/proc/devfs 不需要后端设备)。`flags` 收下但全局注释明说「accepted for Linux ABI parity but not yet modelled」——这一行先记下,边界节再展开。

接下来四类分支,贴前两类(tmpfs + proc/devfs)看全貌:

```cpp
// ---- tmpfs: heap-allocated per mount; owned (sys_umount2 frees the tree) ----
if (strcmp(fstype, "tmpfs") == 0) {
    std::unique_ptr<TmpFs> tfs(new TmpFs());
    auto                   m = tfs->mount();
    if (!m.ok()) { /* ... */ return -to_errno(m.error()); }
    FileSystem* fs = tfs.release();
    if (!vfs_mount_add(target, fs, /*owned=*/true)) {
        delete fs;  // mount table did not take ownership; reclaim
        return -kEnomem;
    }
    return 0;
}

// ---- proc / devfs: boot singletons.  Mounting at a second path shares the
// one instance; owned=false means sys_umount2 detaches the mount point but
// never frees the singleton (it outlives any one mount point, as at /proc).
if (strcmp(fstype, "proc") == 0 || strcmp(fstype, "devfs") == 0) {
    FileSystem* fs = (strcmp(fstype, "proc") == 0)
                         ? static_cast<FileSystem*>(cinux::fs::procfs::instance())
                         : static_cast<FileSystem*>(cinux::fs::devfs::instance());
    if (fs == nullptr) {
        return -kEnodev;  // the FS was never initialised at boot
    }
    if (!vfs_mount_add(target, fs, /*owned=*/false)) { /* ... */ return -kEnomem; }
    return 0;
}
```

(`sys_mount.cpp:52-84`,有删节——省略了 `kprintf` 错误日志。)tmpfs 那一行 017 章讲透了——堆 `new TmpFs`、`mount()` 成功才 `release()` 交裸指针给挂载表、`owned=true` 让 `umount2` 走 `free_tree` 回收整棵树。这里只贴代码回顾链路,RAII 细节不重讲。

proc/devfs 这一行最值得停下来看。它**不 new 任何东西**——取的是 boot 单例:

```cpp
FileSystem* fs = (strcmp(fstype, "proc") == 0)
                     ? static_cast<FileSystem*>(cinux::fs::procfs::instance())
                     : static_cast<FileSystem*>(cinux::fs::devfs::instance());
if (fs == nullptr) {
    return -kEnodev;  // the FS was never initialised at boot
}
```

(`sys_mount.cpp:73-78`。)`procfs::instance()`(`procfs.hpp:147`)和 `devfs::instance()`(`devfs.hpp:223`)是两个 boot 单例的访问器。它们的实现都长这样:**没 init 过就返 `nullptr`**——`ProcFs::is_mounted()`(`procfs.hpp:97`,看 `mounted_` flag)、`DevFs::is_mounted()`(`devfs.hpp:139`,看 `node_count_ > 0`)。工厂看到 `nullptr` 直接返 `-kEnodev`,**拒挂半个未初始化的单例**。这是工厂方法的一个重要纪律:产品还没造好,工厂不发货。

第四类兜底分支:

```cpp
// ---- ramfs (no such FS) / fat / xfs / ... : not supported.  Linux returns
// ENODEV for an unknown filesystem type.
(void)source;
(void)flags;
kprintf("[SYS_MOUNT] unknown filesystem type '%s'\n", fstype);
return -kEnodev;
```

(`sys_mount.cpp:123-128`。)`ramfs` 在 Cinux 压根不存在,`fat`/`xfs` 这些超范围,统一 `-kEnodev`——这是 Linux `mount(2)` 对未知 fstype 的语义。

> **fstype 字符串就是工厂 selector**。把「选哪个 FS」从「怎么造 FS」里剥出来,是这套设计的核心抽象:调用方只扔一个字符串进来,工厂内部分派到四条腿,每条腿自己决定是 new、是取单例、是走块设备链、还是返错。日后加新 FS(nfs 之类),只在工厂里加一个 `strcmp` 分支就行,不用动调用方。这是「工厂方法」的常见误解:工厂不一定每次都造新对象,**取共享单例也是合法的工厂产品**——`procfs::instance()` 就是这个 SingletonRegistry 的味道。`owned=false` 是这个共享语义在挂载表层的一个 bool 投影:挂载表不拥有这个对象,只登记一个指针。
