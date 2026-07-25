---
title: 06 · 主线五:配套纪律 1——unlink 的 indirect 快照
---

# 主线五:配套纪律 1——unlink 的 indirect 快照

## 主线五:配套纪律 1——unlink 的 indirect 快照

迁移之外,还有一个由「`block_buf_` 共享」派生出的隐患,治法不是 `KmBuf` 而是**快照**。

场景:`unlink` 释放大文件的间接块时,要遍历 indirect 指针数组去 free 每个数据块。可 `free_block()` 自己会 `read_block(bitmap) + write_block(bitmap)`——bitmap 块也走 `block_buf_`(老逻辑下)。如果不快照,free 第一个数据块时,`free_block` 的 bitmap RMW 就把 `block_buf_` 里的 indirect 数组覆盖成了 bitmap 字节,回头再读 `indirect[1..]` 读到的就是 bitmap 字节、被当块号——「group out of range」的垃圾块号,free 到根本不存在的块上。

治法:先把整个 indirect 指针数组 `read_block` 进**实例级快照缓冲**,再开始 free 数据块:

```cpp
/// Snapshot buffers for unlink()'s indirect-block release.  free_block()
/// does its own read_block(bitmap)+write_block(bitmap), which overwrite
/// block_buf_; so unlink must copy each indirect pointer array out of
/// block_buf_ BEFORE freeing the data blocks it lists -- otherwise every
/// entry after the first free_block() reads bitmap bytes reinterpreted as
/// a block number (the "group out of range" garbage seen when a file that
/// spans indirect blocks is unlinked).  Two buffers because the doubly-
/// indirect walk is nested: the top-level array must survive while each
/// child's array is processed, so they cannot share one buffer.
uint32_t unlink_ptr_buf_[1024];
uint32_t unlink_child_buf_[1024];
```

([ext2.hpp:447-457](../../../libs/ext2/ext2.hpp#L447))

single-indirect 的释放:

```cpp
// Free singly-indirect block and its referenced data blocks.
// SNAPSHOT discipline: read the indirect pointer array straight into
// unlink_ptr_buf_ (SMP-safe dst overload) BEFORE freeing the data
// blocks it lists.  free_block() does its own read/write of the bitmap
// block; snapshotting the array first keeps every entry intact across
// those later I/Os ...
if (read_block(indirect_blk, unlink_ptr_buf_)) {
    for (uint32_t i = 0; i < ptrs_per_block; ++i) {
        if (unlink_ptr_buf_[i] != 0) {
            free_block(unlink_ptr_buf_[i]);
        }
    }
}
```

([ext2_directory.cpp:402-424](../../../libs/ext2/ext2_directory.cpp#L402))

double-indirect 是嵌套 walk:外层数组得在「处理每个 child 数组」的整个过程中存活,所以**两个快照缓冲**——`unlink_ptr_buf_` 装顶层、`unlink_child_buf_` 装每个 child:

```cpp
// Two-level snapshot: unlink_ptr_buf_ holds the top-level array across the
// outer loop; unlink_child_buf_ holds each child's array inside the inner loop
// (they cannot share a buffer -- the outer array must survive inner processing).
if (read_block(di_blk, unlink_ptr_buf_)) {
    for (uint32_t i = 0; i < ptrs_per_block; ++i) {
        uint32_t child_blk = unlink_ptr_buf_[i];
        if (child_blk == 0) { continue; }
        if (read_block(child_blk, unlink_child_buf_)) {
            for (uint32_t j = 0; j < ptrs_per_block; ++j) {
                if (unlink_child_buf_[j] != 0) {
                    free_block(unlink_child_buf_[j]);
                }
            }
        }
        free_block(child_blk);
    }
}
```

([ext2_directory.cpp:426-457](../../../libs/ext2/ext2_directory.cpp#L426))

这里要诚实说一个**还没收尾的口子**。`unlink_ptr_buf_` / `unlink_child_buf_` 这两块快照是**实例级共享**的——它和 `block_buf_` 同病。快照治的是「同一次 unlink 内部,free 数据块的过程会 clobber 正在遍历的 indirect 数组」这一层 clobber(这是本章关心的、已经治住的那一类);但它**不治**「两个 CPU 同时对同一 ext2 实例上不同路径的文件并发 unlink」这一层——核对 `sys_unlink.cpp`,从 `parent->ops->unlink(...)` 进来到 `Ext2::unlink` 遍历 indirect,全程没有 per-inode / per-fs 的锁把 unlink 串行化;`Ext2::unlink` 自身在遍历这两块快照时也不持 `inode_cache_lock_` 或 `block_alloc_lock_`(`block_alloc_lock_` 只在 `free_block` 内部保护 bitmap RMW,覆盖不到快照缓冲)。也就是说:跨 inode 的并发 unlink 会让两个 CPU 同时读写这两块实例级快照,留下一个真实的残留 race。

这是和「没搭 host TSAN 回归」并列的已知缺口——本章把「单次 unlink 内部的 snapshot clobber」治干净了(对应 `free_block` 的 bitmap RMW 不再覆盖 indirect 数组),但「跨 unlink 的快照缓冲共享」没收。诚实的收法是在 `Ext2::unlink` 入口加一把 per-instance `unlink_lock_`、把整个 indirect 释放串行化(类似 `block_alloc_lock_` 的做法),这一步留 follow-up,不在这章的 `block_buf_` 治理范围内。这章讲的「快照」纪律只覆盖前一层 clobber,别把它误读成「整条 unlink 路径已 SMP-safe」。

> **为什么这里用快照而不用 `KmBuf`?** 因为 free 数据块的过程本身是多次 read/write bitmap,中间不可能每次都重新读 indirect(indirect 在 free 完第一个块后可能本身也要被释放)。快照是「先把要遍历的东西固化下来再动手」这个通用并发纪律的具体实现——「边读边改同一份」这类隐患,通用招数就是先把要读的那份拷出来。
