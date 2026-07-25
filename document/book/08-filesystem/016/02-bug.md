---
title: 02 · 主线一:病灶——block_buf_ 共享 scratch
---

# 主线一:病灶——block_buf_ 共享 scratch

## 主线一:病灶——`block_buf_` 共享 scratch,SMP clobber → wild 块号 → segfault

光说「会 race」太虚,咱们拆一个具体崩点。

`Ext2` 实例上有这么一个成员:

```cpp
/// Scratch block buffer for read_block()/write_block() (max ext2 block = 4096 B)
uint8_t block_buf_[4096];
```

([ext2.hpp:444-445](../../../libs/ext2/ext2.hpp#L444))所有「块级」I/O——读 indirect 指针数组、读 bitmap、写目录块——都要经它过水。`read_block(blk)` 的实现就是往 `block_buf_` 里灌:

```cpp
bool Ext2::read_block(uint32_t block_num) {
    return read_block(block_num, block_buf_);  // shared-buffer variant (NOT SMP-safe)
}
```

([ext2_init.cpp:47-49](../../../libs/ext2/ext2_init.cpp#L47))

现在想象两 CPU 同时跑:

- **CPU A** 走 demand page read,要解析某个文件的 indirect 指针。它进 `resolve_disk_block_`,把 indirect 块 `read_block` 灌进 `block_buf_`,准备读 `indirect[i]` 拿数据块号;
- **CPU B** 这时要分配一个新块,走 `alloc_block` 的 bitmap RMW,也 `read_block` 把 bitmap 块灌进**同一个 `block_buf_`**,把 A 的 indirect 数据覆盖成了 bitmap 字节;
- **CPU A** 回来读 `indirect[i]`,读到的不是 indirect 指针而是 bitmap 的某个字节——这串字节被当成块号,很可能是个**远大于卷块数的 wild block 号**;
- wild block 号交给 NVMe,`dev_->read_blocks` 越界,`[EXT2] read_block(N) I/O failed`;demand page 拿不到数据,`#PF` 收不了尾 → segfault。

这条因果链上,崩点(`read_block(N) I/O failed` 的那条日志)和病灶(`block_buf_` 被 CPU B 覆盖)隔了 `resolve_disk_block_` → `read_block` → `dev_->read_blocks` 好几层调用。在 QEMU 里看到 segfault,回头翻日志,大概率只会怀疑 NVMe 驱动或者 bitmap 逻辑——猜一圈都猜不到头顶那块共享 buffer。头一回撞这种崩点,往往也是在 bitmap 逻辑和 NVMe 驱动里转半天,压根不往头顶那块共享 buffer 上想;真正能把思路掰过来的,是把它搬到 host 上让 TSAN 直接喊出「线程 A 读、线程 B 写同一块内存」那一刻。这正是源码里那条注释想拦住的东西:

```cpp
// SMP-safe per-call scratch (block_buf_ is shared/non-thread-safe; two CPUs
// demand-page-reading would clobber each other -> wild block numbers). Heap
// not stack: #PF runs on IST2 which is only 4 KB (IRQ_STACK_PAGES=1).
```

([ext2_common.cpp:94-96](../../../libs/ext2/ext2_common.cpp#L94))

三件事必须讲清:

1. **这是「共享 scratch clobber」型 race,不是数据结构 race**。加锁治不了——给 `block_buf_` 加一把锁让两 CPU 排队用行不通,因为 `resolve_disk_block_` 读 indirect 的过程本身就是「读一块、解析它、再读下一块」的多步序列,中间夹着别的逻辑,全程持锁代价太大、也容易死锁。引用计数也治不了——这块 buffer 不是被「释放」,是被「合法地写别的数据」覆盖。唯一根治是**消除共享**:每条 SMP 路径自己带一块 buffer。
2. **它是隐性的**。单核测试永远抓不到,只有 SMP 负载(demand page + bitmap alloc 并发)才暴露。014 那条 race-detect 红线提醒过这类——「单核绿不代表 SMP 绿」。
3. **QEMU forensics 容易漏**。崩点离病灶隔了好几层,得多走运才能在日志里把因果串起来。真正能秒抓的工具是 **TSAN**——它直接报「这块内存在线程 A 读、线程 B 写」。可上 TSAN 的前提是代码能在 **host 上脱开 QEMU 跑**——这正是「搬独立库」这步的动机回环。
