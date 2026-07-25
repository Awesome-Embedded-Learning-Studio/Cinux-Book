---
title: 03 · 块设备挂载链:从 /dev/sda 到 Ext2 实例
---

# 块设备挂载链:从 /dev/sda 到 Ext2 实例

## 块设备挂载链:从 /dev/sda 到 Ext2 实例

这一节是本章的**独特核心**——015 VFS 收尾章明确留下没讲的「`sys_mount` 怎么挂一块真盘」,就在这 30 行里。完整贴出来:

```cpp
// ---- ext2 / ext4: source is a block-device path (e.g. /dev/sda).  Resolve
// it to an Inode, pull the IBlockDevice via block_device(), and mount a fresh
// Ext2 over it.  NoFollow: /dev/sda is the device node itself, not a symlink
// to follow.  ENXIO (Linux) when source is not a block device.
if (strcmp(fstype, "ext2") == 0 || strcmp(fstype, "ext4") == 0) {
    if (source == nullptr || source[0] == '\0') {
        return -kEinval;
    }
    auto lr = cinux::fs::vfs_lookup(source,
                                    static_cast<uint32_t>(cinux::fs::LookupFlag::NoFollow), "/");
    if (!lr.ok()) {
        return -to_errno(lr.error());
    }
    cinux::fs::Inode*                ino = lr.value().target;
    cinux::drivers::IBlockDevice*    dev =
        (ino != nullptr && ino->ops != nullptr) ? ino->ops->block_device(ino) : nullptr;
    if (ino != nullptr) {
        cinux::fs::inode_unref(ino);  // vfs_lookup returned a ref
    }
    if (dev == nullptr) {
        return -kEnxio;  // source resolves, but is not a block device
    }
    std::unique_ptr<cinux::fs::Ext2> ext2(new cinux::fs::Ext2(dev));
    auto                             m = ext2->mount();
    if (!m.ok()) { /* ... */ return -to_errno(m.error()); }
    FileSystem* fs = ext2.release();
    if (!vfs_mount_add(target, fs, /*owned=*/true)) { /* ... */ delete fs; return -kEnomem; }
    return 0;
}
```

(`sys_mount.cpp:90-121`,有删节——省略了 `kprintf` 错误日志。)拆成三层。

**第一层:`vfs_lookup(source, NoFollow)` 把字符串变成 `Inode`。** `/dev/sda` 这个路径,走标准的 VFS 路径解析(就是 015 章讲的那套 `vfs_lookup`),拿到带 ref 的 `Inode*`。这里 `NoFollow` 这个 flag 是关键——`LookupFlag::NoFollow = 1u << 5`(`vfs_lookup.hpp:39`),它的语义是「**末段是 symlink 也不要 follow**」。`/dev/sda` 是设备节点本身,不是软链;但语义上必须标 `NoFollow`,否则 Follow 模式下若末段偶为 symlink(想象有人把 `/dev/sda` 做成指向别处的软链),会被解析到非块设备目标,`block_device()` 返 `nullptr`,误判 `ENXIO`。和 `readlink`/`lstat` 用 `NoFollow` 同一原理:**要看节点本身,不看它指向哪**。

**第二层:`ino->ops->block_device(ino)` 调 `InodeOps` 虚方法抽 `IBlockDevice`。** 这一步是整条链最反直觉的设计。把 `IBlockDevice*` 暴露给 `sys_mount` 的,不是某个块设备 API,而是 `InodeOps` 的一个**虚槽**:

```cpp
cinux::drivers::IBlockDevice* dev =
    (ino != nullptr && ino->ops != nullptr) ? ino->ops->block_device(ino) : nullptr;
if (ino != nullptr) {
    cinux::fs::inode_unref(ino);  // vfs_lookup returned a ref
}
if (dev == nullptr) {
    return -kEnxio;  // source resolves, but is not a block device
}
```

(`sys_mount.cpp:100-107`。)`vfs_lookup` 返回的 `Inode` 带一份引用计数(`inode_ref`),这里必须 `inode_unref` 平账——否则引用计数泄漏,块设备节点的 inode 永远释放不掉。拿到 `dev` 之后判空:`nullptr` 就返 `-kEnxio`。

**第三层:`new Ext2(dev)` 把 `IBlockDevice` 喂给 Ext2 构造,真跑 `mount()` 读超级块。**

```cpp
std::unique_ptr<cinux::fs::Ext2> ext2(new cinux::fs::Ext2(dev));
auto                             m = ext2->mount();
if (!m.ok()) { /* ... */ return -to_errno(m.error()); }
FileSystem* fs = ext2.release();
if (!vfs_mount_add(target, fs, /*owned=*/true)) { /* ... */ delete fs; return -kEnomem; }
return 0;
```

(`sys_mount.cpp:108-121`,有删节。)`Ext2` 构造函数签名是 `explicit Ext2(cinux::drivers::IBlockDevice* dev)`(`libs/ext2/ext2.hpp:56`)——只吃一个块设备指针,设备必须比 Ext2 实例活得长(注释明说)。`unique_ptr` RAII 守到 `mount()` 成功才 `release()` 交裸指针给挂载表,跟 tmpfs 那条腿是同一个套路。`mount()` 内部读超级块、块组描述符、inode 表——那是 016 章的范围,这里只点到链路存在 + 端到端测试通过。

> **三个 errno 的精确分工,别混。** `ENXIO`(`kEnxio = 6`,`errno.hpp:27`)是 source **解析得通但不是块设备**——`block_device()` 返 `nullptr`,Linux 约定「要求块设备但给了非块设备」正是 `ENXIO`,**不是 `ENODEV`**(`ENODEV` 是「未知 fstype」,语义不同)。`ENOENT`(`kEnoent = 2`,`errno.hpp:23`)是 source 路径**根本不存在**——`vfs_lookup` 返 `NotFound`,工厂经 `to_errno` 映射成 `ENOENT`。`EINVAL`(`kEinval = 22`,`errno.hpp:38`)是 ext2/ext4 没给 source(`source == nullptr || source[0] == '\0'`,在 `:91-93` 第一道门就拦下)。三码互斥,各自定位流水线上不同的故障点——写测试断言时别用错。
>
> 注意 **ext4 走的是同一个分支**。Cinux 没有 `Ext4` 这个类——`strcmp(fstype, "ext4") == 0` 命中的也是 `new Ext2(dev)`。extent-mapped inode(EXT4 的标志特性)靠 inode flag 路由:`inode_has_extent_tree` 检查 `i_flags & EXT4_EXTENTS_FL`(`ext2_extent.hpp:26-28`),命中就走 extent 读路径,否则走传统 indirect block。所以 `sys_mount -t ext4` 在 Cinux 跟 `-t ext2` 是一回事,只是 fstype 字符串不同。extent tree 内部(depth-0 leaf 限制、index node depth>0 返 Unsupported 停读)是 016 章/ext2 卷的范围,本章只点 ext4 复用 Ext2 的**根因**:inode flag 路由,不另起炉灶。
