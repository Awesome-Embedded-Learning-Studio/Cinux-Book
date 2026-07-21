---
title: 039 · AHCI DMA 迁移
---

# 039 · AHCI 驱动的 DMA 收编:从散装手动,到 DmaPool/PrdtBuilder

> 038 立了 `AHCIBlockDevice` 适配器,但当时笔者留了一句:"`ahci.cpp` 本体的 DMA 迁 DmaPool 是 F5-M1 的活"。这一章就是那个 F5-M1——把 `ahci.cpp` 里剩下的散装 DMA 收进 037 的 `DmaPool`/`PrdtBuilder`,顺手补上 `IDENTIFY`/`FLUSH`,让 `AHCIBlockDevice` 的 `block_count()` 报出真容量、`flush()` 真下发命令。block device → AHCI DMA 这条栈到此闭环。
>
> B 档为主(驱动内部重构 + 两个新 ATA 命令),验证靠构建 + 测试 + 看 `block_count()>0` 真机过。

## 这章咱们要点亮什么

四件事,都围着"`ahci.cpp` 的 DMA 换成 037 那套"转:

1. **setup_port 收编**:command list / FIS / command table 的手动 `g_pmm`+`g_vmm.map`+硬编码偏移,换成 `g_dma_pool` 的 `DmaBuffer`(每端口一份)。
2. **PRDT scatter-gather**:`execute_command` 里手写的单段 `prdt[0]`,换成 `PrdtBuilder`(按 4MB 拆段、循环填、IRQ 标在最后一段)。
3. **`execute_command` 参数化**:`bool write_cmd` 换成 `uint8_t command`,read/write/identify/flush 走同一条命令路径。
4. **IDENTIFY / FLUSH**:`AHCI::identify`(0xEC,读 words 60-61 的 28-bit 容量)+ `AHCI::flush`(0xEA,无 data);`AHCIBlockDevice` 接上真值。

## 收编的是什么

038 之前,`ahci.cpp` 内部自己搞 DMA,和 F1-M3 收掉的、ext2 自带的那套是同一种病:

```cpp
// 散装:每个需要的缓冲都手动来一遍
auto phys = g_pmm.alloc_pages(...);
auto virt = phys + 0xFFFFFFFF80000000ULL;   // 硬编码 direct-map 偏移
g_vmm.map(virt, phys, ...);
// ... 以及 execute_command 里手写 prdt[0] 单段
```

AHCI 还特别讲究**布局**:command list(32 个 header)+ command table 必须在一个 4KB 区域、按硬件要求的结构排布;FIS(接收区)单独一块。这些布局要求不能动——动了硬件就不认。所以这一弧的活是**只换 DMA 来源、不动布局**。

## setup_port:DmaBuffer 接管,布局不动

`kernel/drivers/ahci/ahci.cpp` 里,每个端口的 command list 和 FIS 现在都从 `g_dma_pool` 来:

```cpp
// ahci.cpp:135 / :150
auto cmd_list = cinux::drivers::dma::g_dma_pool.alloc(cinux::arch::PAGE_SIZE);  // 4KB:32 header + table
auto fis       = cinux::drivers::dma::g_dma_pool.alloc(cinux::arch::PAGE_SIZE);
```

关键点:**只换 DMA 来源**。以前是手动 PMM+VMM+硬编码偏移搞出 `cmd_list_phys_/fis_buf_phys_`;现在换成 `DmaBuffer`(move-only,RAII),布局——32 个 command header + command table 挤在一个 4KB 里——原样保留。`DmaBuffer` 给的 `virt = phys + KERNEL_VMA`(direct-map)正好等价于以前那个硬编码偏移,所以硬件看到的总线地址没变,低风险。

> 同一个 GOTCHA 又出现了(037 的 #7):这些 `DmaBuffer` 析构释放时**只 free 物理页,绝不 unmap**——direct-map 的 PTE 是永久的,unmap 它就拆了永久槽,demand paging 死循环。`DmaPool` 已经把这条纪律焊死了,这里直接复用就行。

## PRDT:从单段到 scatter-gather

以前的 `execute_command` 手写一个 `prdt[0]`——单段物理区,意味着一次命令只能传一块连续内存。要是上层给的是一个跨多段的缓冲(后面 Page Cache 会这么干),就得自己拆。

现在换成 037 的 `PrdtBuilder`(设备无关的 scatter-gather 构建器):

```cpp
// ahci.cpp:240
cinux::drivers::dma::PrdtBuilder<MAX_PRDT_ENTRIES> prdt;
// 按 AHCI 的段上限拆分,循环填 prdt,把 IRQ 标在最后一段
```

`PrdtBuilder` 按段上限自动拆分(一块大缓冲 → 多个 ≤4MB 的段),输出通用的 `DmaSegment`,AHCI 这边转成硬件的 PRDT 格式。以后 NVMe/VirtIO 用同一个 `PrdtBuilder`,各转各的硬件格式。

## execute_command 参数化 + IDENTIFY/FLUSH

这一弧还顺手把命令路径统一了。以前 `execute_command` 吃一个 `bool write_cmd`(只区分读/写);现在吃 `uint8_t command`:

```cpp
// ahci.cpp:216
bool AHCI::execute_command(uint8_t port, uint8_t slot, uint8_t command,
                           uint64_t lba, uint16_t count, ...);
// ahci.cpp:189
void AHCI::build_cfis(HBACommandTable* cmd_tbl, uint8_t command, uint64_t lba, uint16_t count);
```

于是 read/write/identify/flush 都走**同一条命令路径**,只是 `command` 字节不同:

- `READ_DMA_EXT` / `WRITE_DMA_EXT`——正常读写;
- `identify` = `0xEC`,读 1 个 sector 的设备信息,从 words 60-61 解析出 **28-bit 容量**(`ahci.hpp:108 ErrorOr<uint64_t> identify(port)`);
- `flush` = `0xEA`,无 data(PRDT `add(0,0)` → 0 段),刷设备写缓存(`ahci.hpp:114 ErrorOr<void> flush(port)`)。

## AHCIBlockDevice 接真值

038 里 `AHCIBlockDevice::block_count()` 是 M4 占位返 0、`flush()` 默认空。这一弧把它们接上真东西:

```cpp
// ahci_block_device.cpp:34 —— create 时 identify 填 capacity_blocks_
auto cap = ahci.identify(port_index);
// ahci_block_device.cpp:47 —— flush override 调真命令
ErrorOr<void> AHCIBlockDevice::flush() { return ahci_->flush(port_index_); }
```

`block_count() > 0` 在真机过,等于顺带验证了 **QEMU AHCI 的 ATA IDENTIFY 工作正常、28-bit 容量(words 60-61)解析对**——这是个比单元测试更有说服力的端到端证据。

## 遗留(明说,不藏着)

- **仍轮询 CI 位**:`execute_command` 发完命令还是 spin 等_CI 位,不是中断驱动。中断驱动是 todo 目标 3,但这一弧 propose 时就排除了(留后续)。轮询在单核 + 阻塞 IO 下够用,上 SMP + 高并发才会成瓶颈。
- **28-bit 容量**:当前只解析 words 60-61(28-bit,够 ext2.img 这种小盘)。大磁盘要升到 words 100-103(48-bit LBA)。现在用不上,留着。

> 这两条都是"知道欠着、但不在这个里程碑还"的债——记清楚比假装没有强。

## 验证

```bash
# ahci.cpp 用上了 DmaPool + PrdtBuilder
grep -nE 'g_dma_pool|PrdtBuilder' kernel/drivers/ahci/ahci.cpp
# identify / flush / block_count 接真值
grep -rn 'identify\|flush\|capacity_blocks_' kernel/drivers/ahci/
```

构建 + 内核测试(这一弧重构 + 加 identify/flush,测试数不变,仍 705/0):

```bash
cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"   # 别接管道
cmake --build build --target run-kernel-test
```

关键的端到端验证是 `AHCIBlockDevice::block_count() > 0` 真机过——它证明 IDENTIFY 全链路(发命令 → DMA 读 1 sector → 解析 words 60-61)都对。

## 小结与下一站

block device → AHCI DMA 这条栈闭环了:038 让 ext2 认 `IBlockDevice*`,039 让 AHCI 用 `DmaPool`/`PrdtBuilder` 干活、还能 IDENTIFY/FLUSH。以后 NVMe/VirtIO 接进来,复用的是同一套契约(`IBlockDevice` + `DmaPool` + `PrdtBuilder`),各转各的硬件格式。

下一站**离开驱动**,进 **F2 内存弧**:040 立 VMA、上 mmap,补 05-memory 卷停在 address space 的那半截。
