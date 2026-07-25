---
title: 01 · 问题在哪,与抽一个最小接口
---

# 问题在哪,与抽一个最小接口

> 假设你现在想给内核接一块 NVMe 盘,或者一个 VirtIO-blk。你去翻 ext2 文件系统的代码,发现它的构造函数长这样:`Ext2(AHCI&, port)`——它**直接吃一个 AHCI 控制器引用**。也就是说,文件系统认识"AHCI"这个具体的硬盘控制器。你想接别的盘?改 ext2。
>
> 这就是分层的错位:文件系统这一层,不该知道底下的盘是什么控制器。这一章立一个**块设备接口** `IBlockDevice`,让 ext2 只对"一个能按块读写的设备"说话,把"具体怎么读写"下放给各个驱动的适配器。从此 ext2 一行不用改,就能换 AHCI、NVMe、VirtIO 任何一种盘。

## 问题:耦合长在哪

037 之前,ext2 和 AHCI 绑得很深,至少三处:

- **构造函数吃 `AHCI&`**:ext2 直接持有一个 AHCI 控制器引用,知道它是 AHCI;
- **自带一摊 DMA**:ext2 自己 `alloc_pages` + 硬编码偏移 + 手动映射,搞一套 DMA 缓冲(和 037 收掉的散装 DMA 同一种病,只不过这份长在 ext2 里);
- **读写直调 AHCI**:`ext2.read_block` 直接调 `AHCI::read`。

能跑,但**文件系统层知道了太多控制器的细节**。后果是接第二种盘要改 ext2 本体、DMA 逻辑和 AHCI 各写一份、后面想做 Page Cache 找不到统一的"读一块"入口。

## 解法:抽一个最小接口

立一个接口,让文件系统只对它说话。`kernel/drivers/block_device.hpp`:

```cpp
class IBlockDevice {
public:
    // 读写 [block, block+count) 这段设备块到/从 buf。buf 是普通虚拟地址。
    // 失败返 ErrorOr<void>——这是个会失败的 IO 操作,值得有名字有类型,不是 bool。
    virtual ErrorOr<void> read_blocks(uint64_t block, uint64_t count, void* buf) = 0;
    virtual ErrorOr<void> write_blocks(uint64_t block, uint64_t count, const void* buf) = 0;
    // 块数 / 块大小 / 刷写
    ...
};
```

两个设计决定:

**接口走 `ErrorOr<void>`,不走 `bool`。** 和 036 一脉相承——这是个会失败的 IO,失败原因(IO 错)该有名字、有类型,而不是一个 `false` 让调用方猜。它是纯内核内部接口(不跨 syscall trap),所以直接用 `ErrorOr`,不用像 syscall 关口那样翻成 `-errno`。

**"最小同步"。** 只做"给块号、读/写、返结果"。请求队列、异步 IO 这些先不碰(留以后),现在只要最朴素的同步语义。抽象要克制,先满足眼前真实需求(一个统一的读块入口),别为想象中的需求过度设计。

## 两个实现

接口立好,给两个实现:

**`RAMBlockDevice`**(`kernel/drivers/ram_block_device.hpp`)——用一块内存当"盘"。它干两件用:一是当**测试桩**(单测里给 ext2 喂一块假盘,不依赖真 AHCI,快又确定);二是给 ramdisk 当后备。

**`AHCIBlockDevice`**(`kernel/drivers/ahci/ahci_block_device.hpp`)——把老的 `AHCI::read/write` 包成 `IBlockDevice`。它持一个 DMA 缓冲(从 037 的 `DmaPool` 来),`read_blocks` 走 DMA 把数据搬到调用方缓冲。注意它**不碰 `ahci.cpp` 本体**——只是在 AHCI 外面包一层适配器。
