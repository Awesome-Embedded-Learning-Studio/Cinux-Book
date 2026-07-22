---
title: 039 · AHCI DMA 迁移
---

# 039 · 让 AHCI 驱动真正用上 DMA 池:从单段轮询,到 scatter-gather

> 037 造好了 DMA 池(`DmaPool`/`PrdtBuilder`),038 把 AHCI 包成了 `IBlockDevice` 适配器。但那个适配器底下——`ahci.cpp` 驱动本体——还在用**自己手写的一套 DMA**:手动分配、硬编码偏移、单段物理区。也就是说,池子有了、接口有了,可真正搬数据的驱动还在用老办法。这一章把 `ahci.cpp` 的 DMA 也迁到 037 那套池/构建器上,顺带补两个它一直缺的 ATA 命令(`IDENTIFY` 读盘容量、`FLUSH` 刷写缓存),让 `AHCIBlockDevice` 的 `block_count()`/`flush()` 从占位变成真东西。

## 驱动里还藏着什么散装 DMA

038 之前,`ahci.cpp` 内部自己搞 DMA,和 037 收掉的、ext2 自带的那套是同一种病——每个需要的缓冲都手动来一遍:`alloc_pages` + 硬编码 `+0xFFFFFFFF80000000` 偏移 + 手动映射,发命令时手写一个单段 PRDT(`prdt[0]`)。

AHCI 还特别讲究**布局**:command list(32 个命令头)+ command table 必须挤在一个 4KB 区域、按硬件要求的结构排;FIS(接收区)单独一块。这些布局是硬件硬性要求,不能动——动了控制器就不认。所以这一章的活是**只换 DMA 的来源、不动布局**。

## setup_port:DMA 缓冲改从池子里拿

每个端口的 command list 和 FIS,现在都从 `g_dma_pool` 来:

```cpp
// kernel/drivers/ahci/ahci.cpp
auto cmd_list = g_dma_pool.alloc(PAGE_SIZE);   // 4KB:32 个命令头 + command table
auto fis       = g_dma_pool.alloc(PAGE_SIZE);   // FIS 接收区
```

关键:**只换 DMA 来源,布局原样保留**。以前手动 PMM+VMM+硬编码偏移;现在换成 move-only、RAII 的 `DmaBuffer`。它给的 `virt = phys + KERNEL_VMA`(direct-map)正好等价于那个硬编码偏移,所以硬件看到的总线地址没变——低风险。而 037 那条"direct-map 的页表项绝不 unmap"的纪律,`DmaPool` 已经焊死了,这里直接复用。

## PRDT:从单段到 scatter-gather

以前 `execute_command` 手写一个 `prdt[0]`——单段物理区,一次命令只能传一块连续内存。要是上层给的是跨多段的缓冲(后面 Page Cache 会这么干),就得自己拆。

现在换成 037 的 `PrdtBuilder`(设备无关的 scatter-gather 构建器):它按段上限自动把一块大缓冲拆成多段,输出通用的段描述,AHCI 这边转成硬件的 PRDT 格式。以后 NVMe/VirtIO 用同一个 `PrdtBuilder`,各转各的格式——这套契约是共享的。

## execute_command:一条路走四种命令

这一章还顺手把命令路径统一了。以前 `execute_command` 吃一个 `bool write_cmd`(只分读/写);现在吃 `uint8_t command`,于是 read/write/identify/flush 都走**同一条路径**,只是命令字节不同:

- `READ_DMA_EXT` / `WRITE_DMA_EXT`——正常读写;
- `IDENTIFY`(`0xEC`)——读一个扇区的设备信息,从中解出盘容量(28-bit,够小盘);
- `FLUSH`(`0xEA`)——无数据,刷设备的写缓存。

## AHCIBlockDevice:占位变真值

038 里 `AHCIBlockDevice::block_count()` 还是占位返 0、`flush()` 默认空。这一章接上真东西:`create` 时调 `identify` 填容量,`flush()` 下发真命令。`block_count() > 0` 在真机上跑通,等于顺带验证了"发 IDENTIFY 命令 → DMA 读一个扇区 → 解析容量字段"整条链路都对——这比单元测试更有说服力。

## 诚实地记一笔债

这一章**没做**两件事,明说比藏着强:

- **还是轮询**。`execute_command` 发完命令,靠死等 CI(command issue)位,不是中断驱动。单核 + 阻塞 IO 下够用,真上多核高并发才成瓶颈。中断驱动是后面的事。
- **28-bit 容量**。只解了 IDENTIFY 的 28-bit 容量字段,够 ext2.img 这种小盘;大磁盘要升到 48-bit 字段。现在用不上。

> 抽象的节奏:037 造池、038 立接口、039 让驱动真用上——三步才把"AHCI 的 DMA"这件事收干净。每一步都独立可验证、不破坏前面,这是分层迁移的好处。

## 验证

```bash
# ahci.cpp 用上了 DmaPool + PrdtBuilder(不再是散装)
grep -nE 'g_dma_pool|PrdtBuilder' kernel/drivers/ahci/ahci.cpp
grep -cE '0xFFFFFFFF80000000' kernel/drivers/ahci/ahci.cpp   # 期望:0,硬编码偏移没了
# IDENTIFY / FLUSH / block_count 接真值
grep -rn 'identify\|flush\|capacity_blocks_' kernel/drivers/ahci/
```

构建:

```bash
cmake -B build -S . && cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
```

端到端的证据是 `AHCIBlockDevice::block_count() > 0` 在内核测试里真机过——它证明 IDENTIFY 全链路(发命令 → DMA 读 → 解析容量)都对。想看 scatter-gather 的价值,等后面 Page Cache 给出不连续缓冲时自然就体现出来。
