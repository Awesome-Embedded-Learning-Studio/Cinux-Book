---
title: 05 · 主线四:治法 3——调用方迁移
---

# 主线四:治法 3——调用方迁移

## 主线四:治法 3——调用方迁移,把每条 SMP 路径换成 per-call `KmBuf`

API 给了,接下来是体力活——逐个把 SMP 路径上的调用方迁到双参版 + 自带 `KmBuf`。这是最容易漏的地方,得讲清每个调用方迁了之后消除了哪个具体 clobber 点。

**`resolve_disk_block_`——读 indirect 指针数组那条最敏感的路径**(主线一里 wild 块号的直接来源)。它原本 clobber `block_buf_`,现在加一个 `uint8_t* scratch` 参数,由上层 `Ext2FileOps::read` 传进来:

```cpp
uint32_t Ext2FileOps::resolve_disk_block_(const Ext2Inode& disk, uint64_t file_block,
                                          uint64_t block_ptrs_per_block, uint8_t* scratch) {
    ...
    if (!ext2_.read_block(indirect_block, scratch)) return 0;
    const auto* indirect = reinterpret_cast<const uint32_t*>(scratch);
    uint32_t    blk      = indirect[file_block - EXT2_DIRECT_BLOCKS];
```

([ext2_common.cpp:196-219](../../../libs/ext2/ext2_common.cpp#L196))double-indirect 那段([ext2_common.cpp:221-238](../../../libs/ext2/ext2_common.cpp#L221))同理,二级都用同一个 `scratch`——因为这条解析路径上「读完一级解析完再读下一级」,是顺序的,一块 buffer 够。

上层 `read()` 自己 new 一块 `KmBuf`,出函数自动释放:

```cpp
// SMP-safe per-call scratch ...
KmBuf scratch(4096);
if (!scratch) {
    return cinux::lib::Error::IOError;  // slab OOM
}
...
uint32_t disk_block = resolve_disk_block_(disk, file_block, block_ptrs_per_block, scratch.data());
```

([ext2_common.cpp:97-113](../../../libs/ext2/ext2_common.cpp#L97))

**`get_or_alloc_block`——独立 `zbuf`,不再「写完重读」覆盖父 array**。这条路径上要给新分配的块清零并写盘。老代码用 `block_buf_`,清零那一下就把刚读进来的 indirect 数组覆盖了,所以老逻辑只能「写完再重读父数组」;现在给清零一块独立 `zbuf`,父 array 的 `buf` 原封不动:

```cpp
KmBuf zbuf(4096);
if (!zbuf || !zero_and_write_block(data_blk, zbuf.get())) { free_block(data_blk); return 0; }
// zbuf was a separate buffer, so child_ptrs (the child array in child_buf)
// is still intact -- patch and write.
child_ptrs[idx2] = data_blk;
if (!write_block(child_blk, child_buf.get())) { ... }
```

([ext2_inode.cpp:446-453](../../../libs/ext2/ext2_inode.cpp#L446))。源码注释把这点写得很直白([ext2_inode.cpp:376-381](../../../libs/ext2/ext2_inode.cpp#L376)):「the old shared-block_buf_ code had to re-read the parent each time because zeroing the child clobbered the only buffer」。迁完之后 double-indirect 的两层 walk 各自一块 `KmBuf`(`di_buf` / `child_buf`),互不踩。

**`read_disk_inode` / `write_disk_inode`——各自 `KmBuf`,顺手把 `locate_inode_block` 改成「只算术不读」**。这俩是 RMW 风格(读 inode 所在块、patch inode 槽、写回),原来共用 `block_buf_`,现在各自一块:

```cpp
bool Ext2::read_disk_inode(uint32_t ino, Ext2Inode& out_inode) {
    InodeLoc loc{};
    if (!locate_inode_block(ino, loc)) { return false; }
    KmBuf buf(4096);
    if (!buf) { return false; }
    if (!read_block(loc.target_block, buf.get())) { ... }
    memcpy(&out_inode, buf.data() + loc.within_block_offset, sizeof(Ext2Inode));
    return true;
}
```

([ext2_inode.cpp:48-63](../../../libs/ext2/ext2_inode.cpp#L48))`write_disk_inode` 同款([ext2_inode.cpp:65-87](../../../libs/ext2/ext2_inode.cpp#L65))。

同时,`locate_inode_block` 从「定位 + `read_block` 它」改成**只做算术定位**(算 `target_block` 和 `within_block_offset`),把「读块」交给上层各自的 `KmBuf`:

```cpp
// Pure arithmetic bounds check; the block read is the caller's job
// (read_disk_inode / write_disk_inode each use their own KmBuf so two
// CPUs touching different inodes don't share block_buf_).
```

([ext2_inode.cpp:38-40](../../../libs/ext2/ext2_inode.cpp#L38))。这一改直接去掉一处共享读写——定位逻辑本来就不需要真读块,读是上层的活,让它显式地用自己的 buffer 读。

**bitmap alloc/free** —— 每次进 `alloc_block` / `free_block` 自己 `KmBuf`:

```cpp
KmBuf blk_buf(4096);
if (!blk_buf || !read_block(bitmap_block, blk_buf.get())) { ... }
auto* bitmap = blk_buf.data();
```

([ext2_block.cpp:43-50](../../../libs/ext2/ext2_block.cpp#L43),`free_block` 见 [ext2_block.cpp:124-128](../../../libs/ext2/ext2_block.cpp#L124))。

**`Ext2FileOps::read`** —— 用 `scratch.data()`(主线三已贴)。**directory readdir** —— `Ext2DirOps::readdir` 在 `ext2_dirops.cpp` 里,每个目录块自己 `KmBuf buf(4096)`([ext2_dirops.cpp:73](../../../libs/ext2/ext2_dirops.cpp#L73))。同理,目录写路径 `add_dir_entry`(在另一个文件 `ext2_directory.cpp`)也是每个目录块一块独立 `KmBuf dir_buf`([ext2_directory.cpp:43-50](../../../libs/ext2/ext2_directory.cpp#L43)),逻辑一致。

迁移策略不是一把梭,是**按路径逐个替换 + 每个 `grep` 确认 `block_buf_` 不再出现在 SMP 路径**——这跟 lab 的 grep 验收呼应。最后一公里的风险要讲明:漏一个调用点就是留一个 race。所以 lab 不让读者改代码,而是让读者自己 `grep` 验收无残留——把迁移纪律内化成「能自己判断 block_buf_ 还该出现在哪儿」。
