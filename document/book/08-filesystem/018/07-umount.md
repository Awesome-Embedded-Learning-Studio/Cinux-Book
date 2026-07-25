---
title: 07 · umount2:摘槽 vs delete 的分叉
---

# umount2:摘槽 vs delete 的分叉

## umount2:摘槽 vs delete 的分叉

`do_umount2_kernel` 是 `do_mount_kernel` 的镜像——挂的反面就是摘。代码很短:

```cpp
int64_t do_umount2_kernel(const char* target, [[maybe_unused]] uint64_t flags) {
    // MNT_FORCE / MNT_DETACH / MNT_EXPIRE not yet modelled

    if (target == nullptr || target[0] == '\0') {
        return -kEinval;
    }
    if (!cinux::fs::vfs_mount_remove(target)) {
        return -kEnoent;  // nothing mounted at that path
    }
    return 0;
}
```

(`sys_umount2.cpp:22-32`。)三件事:**(1)** flags 标 `[[maybe_unused]]`,意味着这个参数在函数体里**根本没被读**——纯 ABI 占位;**(2)** target 空/null 返 `-kEinval`;**(3)** `vfs_mount_remove` 返 false(路径不在挂载表里)返 `-kEnoent`。至于「摘槽 vs delete」的分叉,全在上一节贴过的 `vfs_mount_remove` 里——`owned=true` 的(tmpfs/ext2)连 FS 实例一起 delete,`owned=false` 的(boot 静态/proc/devfs 二次挂)只摘点。

上层 `sys_umount2`(`sys_umount2.cpp:34-40`)只做 `resolve_user_path(target)` 一层 SMAP 读取,然后委托 `do_umount2_kernel`:

```cpp
int64_t sys_umount2(uint64_t target_virt, uint64_t flags, uint64_t, uint64_t, uint64_t, uint64_t) {
    cinux::fs::PathBuf target;
    if (!resolve_user_path(target_virt, target.data())) {
        return -kEfault;
    }
    return do_umount2_kernel(target.data(), flags);
}
```

(`sys_umount2.cpp:34-40`。)`resolve_user_path` 是 SMAP 安全读取(`path_util.hpp:65`)——把用户态地址里的路径字符串拷到内核栈,再交给 `do_umount2_kernel` 处理。

> **umount2 的 `MNT_FORCE` 是纯 ABI 占位,别误以为有强制卸载。** grep 全树(`kernel/` 目录)没有任何 `#define MNT_FORCE`/`MNT_DETACH`/`MNT_EXPIRE` 的命中——这几个名字只出现在 `sys_umount2.cpp:23` 的注释里(`// MNT_FORCE / MNT_DETACH / MNT_EXPIRE not yet modelled`)和 `.hpp` 文件头说明里,运行期连 `if (flags & MNT_FORCE)` 这种判断都没有,只走 `vfs_mount_remove(target)` 一条路。这是最容易 overclaim 的地方——本章不能宣称「支持强制卸载」,只能写「接受 flags 为 ABI 兼容,行为等同 `umount(target)`」。`test_mount.cpp` 全程传 `flags=0`(`test_mount.cpp:59`/`:84`/`:96` 等),也没覆盖非零 flags 路径——因为根本没有非零 flags 路径。
