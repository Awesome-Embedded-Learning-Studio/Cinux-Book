---
title: 11 · 收尾:范围与边界
---

# 收尾:范围与边界

## 范围与边界(诚实说)

这一章的 race 治理有几条没收尾的口子,摊开讲清,别让读者读完以为「ext2 已 SMP-safe」。

- **host PAL + TSAN 确定性回归已到位**(原「没搭 host TSAN」那条 deferred,现已收)。主线八讲完:host PAL(`test/unit/ext2_host_pal.cpp`)mock 掉 `kprintf` / `kmalloc` 让 ext2 在 host 上跑真逻辑;`test_ext2_host.cpp` 走完整 VFS 往返(readdir + read + create/write/read-back + unlink);`test_ext2_concurrent.cpp` 4 线程压同一个 `Ext2`,`-DCINUX_HOST_TSAN=ON` 秒级抓 `block_buf_` race。`block_buf_` 治理现在是「逻辑根治 + 回归确定性验证」双重闭环,不再只是 `run-kernel-test` 全绿的概率证据。
- **`unlink` 的跨并发快照缓冲还没治**。主线五末尾已经诚实标注:`unlink_ptr_buf_` / `unlink_child_buf_` 治的是「单次 unlink 内部 free 过程的 clobber」,不治「两个 CPU 并发 unlink 同一 ext2 实例」那层共享快照 race——那层需要一把 per-instance `unlink_lock_`,留 follow-up。这章不假装它已 SMP-safe。
- **truncate 是 shrink-only,孤儿块不回收**。主线七已说,`O_TRUNC` 截断掉的孤儿数据块不释放,是已知 leak(hobby-os 式)。read 不超过 `i_size` 所以非正确性问题,只浪费磁盘;完整的孤儿块回收留 follow-up。
- **只讲「搬独立库 + 治 `block_buf_` 成 SMP-safe」这一根绳**。host PAL 的搭法主线八给了概貌(`kprintf` / `kmalloc` 走 libc + 守 slab 清零契约),但 PAL 设计的完整动机、ASAN/TSAN 开关的取舍不展开;`ext2_dirops.cpp` 这种「搬家顺带的结构整理」也只一句话带过,不教拆分方法论。
- **ext4 extent 读路径只到 depth-0 leaf**。主线九已讲:`resolve_disk_block_` 入口先判 `EXT4_EXTENTS_FL`,extent-mapped 的 inode 走 `extent_lookup_block` 那条 depth-0 leaf 解析(纯算术、不读盘、不碰 buffer,所以不在 `block_buf_` race 的范围内)。但 `eh_depth > 0` 的 index 节点(很大或很碎的文件才会用)`Unsupported`,本驱动 bail 不读——那一层会引入 index 块的 I/O、又得 `KmBuf`,留后续章。
- **目录读写路径(`lookup_in_dir` / symlink readlink 等)的 per-call `KmBuf`**已随这次搬家一起到位,本章按合并态讲,不展开每条目录路径的迁移细节。

> 这一章的 race 修复,`run-kernel-test` 跑全绿是验证证据(不是战绩)。它站得住的真正理由,是每条 SMP 路径都换了 per-call `KmBuf`、`grep` 确认 `block_buf_` 不再出现在并发路径上、配套的 `block_alloc_lock_` 串行了 bitmap RMW、unlink 的 indirect 快照隔离了 free 过程的 clobber——这一套逻辑是闭环的。再加上 host PAL 让 ext2 在 host 上跑真逻辑、`test_ext2_concurrent` 在 TSAN 下并发压 lookup(主线八),`block_buf_` race 有了确定性回归,不再只靠 QEMU 全绿。剩下没收的口子(unlink 跨并发快照、truncate 孤儿块、extent depth>0)都摊在上面,各自留了明确的 follow-up。
