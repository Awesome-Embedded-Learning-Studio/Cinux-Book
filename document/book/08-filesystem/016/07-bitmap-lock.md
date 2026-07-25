---
title: 07 · 主线六:配套纪律 2——block_alloc_lock_ 串行 bitmap RMW
---

# 主线六:配套纪律 2——block_alloc_lock_ 串行 bitmap RMW

## 主线六:配套纪律 2——`block_alloc_lock_`,bitmap RMW 串行

第二类由「`block_buf_` 共享」之外的 SMP 隐患,治法是**锁**不是 buffer。

block/inode bitmap 的分配是 read-modify-write:读 bitmap 块、找 free bit、mark 它、写回。两 CPU 同时分配:都读到同一个 bit 是 free、都 mark 它、都写回——同一个块被分给两个文件,数据损坏(cc1 的 indirect 块被某个 `.o` 的写覆盖,就这么来的)。这跟 `block_buf_` clobber 无关,是 RMW 数据 race。

治法是 `block_alloc_lock_` 这个 `Spinlock` 成员:

```cpp
mutable cinux::proc::Spinlock block_alloc_lock_;  ///< SMP: serialize block+inode bitmap alloc/free
```

([ext2.hpp:496](../../../libs/ext2/ext2.hpp#L496))。`alloc_block` / `free_block` 进去先拿这把锁:

```cpp
uint32_t Ext2::alloc_block() {
    // SMP: bitmap read-modify-write (read bitmap, mark bit, write bitmap) is
    // not atomic -- two CPUs alloc concurrently can both see a bit free, both
    // mark+write, and return the SAME block to two files (one overwrites the
    // other's data, e.g. cc1's indirect block clobbered by a .o write).
    // Serialize under block_alloc_lock_.  Held across disk I/O (alloc is rare
    // relative to data read/write; cache miss is acceptable).
    auto g = block_alloc_lock_.guard();
    ...
```

([ext2_block.cpp:21-29](../../../libs/ext2/ext2_block.cpp#L21))`free_block` 同款([ext2_block.cpp:104-106](../../../libs/ext2/ext2_block.cpp#L104))。

**它和 `KmBuf` 的分工讲清楚**:

- `KmBuf` 治「**读写中间数据(scratch)被覆盖**」——`block_buf_` 这块 buffer 的内容被别的 I/O 冲掉;
- `block_alloc_lock_` 治「**读改写持久数据(bitmap)的 RMW race**」——bitmap 这份持久 metadata 被两 CPU 同时改。

一个治 scratch,一个治 metadata,两个正交,都要有。光上 `KmBuf` 不上锁,bitmap 还是会被分给两个文件;光上锁不上 `KmBuf`,indirect 数组还是会被 bitmap RMW 覆盖。

锁的**粒度**也值得讲一句:这是把整个 alloc/free 操作都串起来(粗粒度),不按 block group 细分。为什么?bitmap 修改是低频操作(分配/释放块远少于数据读写),而且必须串行才正确,不值得为了并发度去按 group 拆锁——**简单正确优先**。
