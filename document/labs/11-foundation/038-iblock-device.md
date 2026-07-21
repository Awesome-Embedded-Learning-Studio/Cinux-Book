---
title: Lab 038 · IBlockDevice 块设备抽象 验证
---

# Lab 038 · IBlockDevice 块设备抽象 验证

> 对应 `document/book/11-foundation/038-iblock-device.md`。验证档 **B 档**(基建/重构)。验证靠构建 + 测试 + 看接口形状 + 看解耦是否到位。

## 目标

确认四件事:

1. `IBlockDevice` 接口在,走 `ErrorOr<void>`(不是 bool);
2. 两个实现都在:`RAMBlockDevice`(内存/测试桩)+ `AHCIBlockDevice`(AHCI 适配器);
3. ext2 真解耦了——构造函数吃 `IBlockDevice*`,自带 DMA 淘汰,`block_buf_[4096]` 固定数组替代;
4. run-kernel-test 从 037 的 694 涨到 705。

## 步骤

### 1. 构建 + 内核测试(取真退出码!)

```bash
git checkout 038_foundation_iblock_device 2>/dev/null || git checkout 038_*
cmake -B build -S . && cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
cmake --build build --target run-kernel-test
```

**期望**:`build=0`(注意别用 `cmake --build ... | tail; echo $?`,那是 tail 的码);run-kernel-test 全绿,总数 ~705。

### 2. 接口 + 实现

```bash
grep -rn 'class IBlockDevice' kernel/drivers/block_device.hpp
grep -nE 'ErrorOr<void> read_blocks|ErrorOr<void> write_blocks' kernel/drivers/block_device.hpp
grep -rn 'class RAMBlockDevice\|class AHCIBlockDevice' kernel/drivers/
```

**期望**:`block_device.hpp:44 class IBlockDevice`,`read_blocks/write_blocks` 返 `ErrorOr<void>`;`ram_block_device.hpp:36`、`ahci/ahci_block_device.hpp:41` 两个实现。

**思考**:为什么 `IBlockDevice` 的方法走 `ErrorOr<void>` 而不是 `bool`?——它是纯内核内部接口(不跨 syscall trap),可以直接用 `ErrorOr`,失败原因(IO 错)有名字有类型,比 `false` 强。对比 syscall 边界那层才翻 `-errno`(见 036)。

### 3. ext2 解耦

```bash
grep -n 'explicit Ext2' kernel/fs/ext2.hpp       # 期望:Ext2(IBlockDevice* dev)
grep -n 'block_buf_\[\|dma_buf_' kernel/fs/ext2.hpp  # 期望:block_buf_[4096] 在,dma_buf_* 全没了
grep -rn 'AHCIBlockDevice::create' kernel/proc/init.cpp  # 期望:init.cpp 接线用 create + Ext2(IBlockDevice*)
```

**思考**:`block_buf_` 为什么用固定数组 `uint8_t block_buf_[4096]` 而不是 heap 分配?——见章节:容纳任何 ext2 block(≤4096),免 heap alloc/析构,DMA 的事下放给 `AHCIBlockDevice`。

### 4.(可选)用 RAMBlockDevice 当桩

去看内核测试里 `RAMBlockDevice` 怎么用的——它给 ext2 喂一块内存假盘,不依赖真 AHCI。这是"测试桩"的标准用法:被测对象(ext2)只认 `IBlockDevice*`,不在乎后面是真盘还是内存。

## 验收清单

- [ ] 构建 `build=0`(真退出码),run-kernel-test 全绿(~705)。
- [ ] `IBlockDevice` 在,`read_blocks/write_blocks` 返 `ErrorOr<void>`。
- [ ] `RAMBlockDevice` + `AHCIBlockDevice` 两个实现都在。
- [ ] ext2 构造吃 `IBlockDevice*`,`block_buf_[4096]` 在,`dma_buf_*` 已删。
- [ ] 能说清「为什么接口走 ErrorOr 不走 bool」「为什么 QEMU AHCI 写测试要挑低位 sector」。

## 别做这些

- **别**在 `cmake --build` 后接管道再读 `$?`——那是 tail 的退出码,会放过编译错。直接 `> log 2>&1; echo $?`。
- **别**假设 QEMU AHCI 盘容量 = ext2.img 文件大小——以 IDENTIFY 几何为准(GOTCHA #8)。
