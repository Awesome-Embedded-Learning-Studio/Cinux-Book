---
title: 04 · 两条挂载通路:差别全在 owned bool
---

# 两条挂载通路:差别全在 owned bool

## 两条挂载通路:差别全在挂载表那个 owned bool

tmpfs 有两条挂进系统的路:boot 静态挂 `/tmp`、运行时 `sys_mount` 堆挂。这俩调的是**同一个 `TmpFs` 类、同一个 `mount()`**(幂等,二次 `mount()` 是 no-op,`tmpfs.cpp:375-403`),代码路径几乎对称。**唯一决定「`umount` 时会不会释放」的,是 `vfs_mount_add` 的第三个参 `owned`**。

boot 通路(`tmpfs_init.cpp`):

```cpp
namespace {
TmpFs g_tmpfs;   // 静态局部,寿命 = 整个内核运行
}

bool tmpfs::init() {
    if (!g_tmpfs.mount().ok()) { ... return false; }
    if (!vfs_mount_add("/tmp", &g_tmpfs)) { ... return false; }   // 默认 owned=false
    kprintf("[TMPFS] mounted at /tmp\n");
    return true;
}
```

(`tmpfs_init.cpp:33-50`。)静态 `g_tmpfs` 的命超表——PID1 busybox init、GCC/cc1/as/ld 写中间 `*.o`/`*.s` 全靠 `/tmp`,挂载表只是中途登记一个指针,对象不能随槽位摘除消失(否则 `sys_umount2("/tmp")` 会 `delete` 静态对象 = use-after-free)。所以 `vfs_mount_add` 走默认 2-arg 版,`owned` 默认 false(`vfs_mount.hpp:47`、`vfs_mount.hpp:79`),挂载表只存指针、不管释放。`umount2("/tmp")` 在 `vfs_mount_remove` 里看到 `owned=false`,只摘槽、不 `delete`(`vfs_mount.cpp:93-99`)。

运行时通路(`sys_mount.cpp`):

```cpp
if (strcmp(fstype, "tmpfs") == 0) {
    std::unique_ptr<TmpFs> tfs(new TmpFs());   // 堆对象
    auto m = tfs->mount();
    if (!m.ok()) { ... return -to_errno(m.error()); }
    FileSystem* fs = tfs.release();
    if (!vfs_mount_add(target, fs, /*owned=*/true)) {   // 关键:owned=true
        delete fs;   // 挂载表没收,自己回收
        return -kEnomem;
    }
    return 0;
}
```

(`sys_mount.cpp:51-67`。)`owned=true` 是关键:它告诉挂载表「这对象是你堆分配的、你有释放义务」。`sys_umount2` → `vfs_mount_remove` 看到 `owned=true` 就 `delete fs`(`vfs_mount.cpp:93-95`),`TmpFs` 析构(`tmpfs.cpp:369-373`)递归 `free_tree`(`tmpfs.cpp:351-363`)释放整棵树。错误腿(`mount()` 失败、表满)都有显式 `delete` 回收,不泄漏。

看起来「对象在哪创建」决定生命周期——boot 在静态区、`sys_mount` 在堆上。**实际上决定生命周期的是「挂载表登不登记所有权」**。一个 bool 字段 + 默认参,既不破坏旧的 `owned=false` 调用者(boot 接线、栈局部 mock 的测试),又让 `sys_mount` 路径能干净回收。这是整个挂载表 ownership 模型的设计核心。

> **boot 误标 owned=true 会炸**。boot static 挂载若误传 `owned=true`,`sys_umount2("/tmp")` 会在 `vfs_mount_remove` 里 `delete` 静态 `g_tmpfs` —— 双重释放/崩溃。所以 `tmpfs::init`、`devfs::init`、`procfs::init` 全用默认 2-arg(`owned=false`)是**刻意的安全约束,不是省事**。这条纪律写进挂载表的注释(`vfs_mount.hpp:42-47`):boot/static 接线永远 `owned=false`,只有 `sys_mount` 这种「对象是调用方 new 出来的、没别的 owner」的路径才传 `true`。

**§14 文件门**。`tmpfs.cpp` 是纯逻辑(无 `kprintf`),为的是能同时链进内核和 host 单测;boot 的 `kprintf("[TMPFS] mounted at /tmp")` 落在独立的 `tmpfs_init.cpp`(`tmpfs_init.cpp:48`)。这是 DevFS/ProcFS 同款的 §14 文件门——boot I/O 跟核心逻辑分 TU,源码零 `#ifdef`,CMake 决定编不编。boot 接线在 `proc/init.cpp` 里,紧跟 `procfs::init` 之后(`init.cpp:145`),顺序是 `devfs::init`(`init.cpp:137`)→ `procfs::init`(`init.cpp:141`)→ `tmpfs::init`(`init.cpp:145`)。
