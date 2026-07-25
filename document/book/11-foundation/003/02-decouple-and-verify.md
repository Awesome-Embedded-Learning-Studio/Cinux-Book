---
title: 02 · ext2 解耦、抽象值在哪、验证
---

# ext2 解耦、抽象值在哪、验证

## ext2 解耦

接口有了,ext2 这边的改动就清爽了:

```cpp
// libs/ext2/ext2.hpp
explicit Ext2(IBlockDevice* dev);   // 构造函数只认接口,不认 AHCI
// ...
IBlockDevice* dev_;
uint8_t block_buf_[4096];            // 自带 DMA 淘汰,换成固定 scratch 数组
```

那一摊自带的 DMA 缓冲全删了,换成一块 4096 字节的固定数组 `block_buf_`(容纳任何 ext2 块,免动态分配)。DMA 的事下放给 `AHCIBlockDevice`——ext2 不再关心"块怎么从盘上搬过来",只说"把第 N 块搬过来"。

接线处(`kernel/proc/init.cpp`):

```cpp
static auto blk_dev = AHCIBlockDevice::create(AHCI::instance(), 1);
static Ext2 ext2(blk_dev.ok() ? &blk_dev.value() : nullptr);
```

`AHCIBlockDevice::create` 返一个 `ErrorOr`(构造可能失败,比如端口没盘),ext2 拿到接口指针就开干。

## 这套抽象值在哪

现在你回头想"接一块 NVMe"那个问题:写个 `NVMeBlockDevice : IBlockDevice`,把 NVMe 的读写包进去,然后 `Ext2 ext2(&nvme_dev)`——ext2 一行没改。AHCI、NVMe、VirtIO 共用的是同一个 `IBlockDevice` 契约(加上 037 的 `DmaPool`/`PrdtBuilder`),各转各的硬件格式。这就是**依赖倒置**的回报:高层(ext2)依赖抽象(`IBlockDevice`),不依赖具体(AHCI);新增一种盘,只新增一个适配器,不动已有代码。

> 小提醒:这是"最小同步"接口。真到高并发 + 异步 IO(请求队列、中断驱动完成),接口形态会再演进(可能加 async/回调)。但那是后面的需求,现在不为它先造一套复杂的。

## 验证

```bash
# 接口 + 两个实现
grep -n 'class IBlockDevice' kernel/drivers/block_device.hpp
grep -rn 'class RAMBlockDevice\|class AHCIBlockDevice' kernel/drivers/
# ext2 解耦:构造函数吃接口、block_buf_ 在、自带 DMA 没了
grep -n 'explicit Ext2(IBlockDevice' libs/ext2/ext2.hpp
grep -n 'block_buf_\[' libs/ext2/ext2.hpp
```

构建:

```bash
cmake -B build -S . && cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
```

想看这套抽象的灵活性:`RAMBlockDevice` 在内核测试里就是现成的例子——它给 ext2 喂一块内存假盘,ext2 跑得好好的,完全不依赖真 AHCI。被测对象(ext2)只认 `IBlockDevice*`,不在乎后面是真盘还是内存——这就是抽象到位的证据。
