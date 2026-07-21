---
title: Lab 039 · AHCI DMA 迁移 验证
---

# Lab 039 · AHCI DMA 迁移 验证

> 对应 `document/book/12-storage/039-ahci-dma.md`。验证档 **B 档**(驱动重构 + IDENTIFY/FLUSH)。验证靠构建 + 测试 + grep + `block_count()>0` 真机。

## 目标

确认四件事:

1. `ahci.cpp` 的 command list / FIS / table 都从 `g_dma_pool` 来了(不再是手动 PMM+VMM);
2. PRDT 用 `PrdtBuilder`(scatter-gather),不再是单段 `prdt[0]`;
3. `execute_command` 参数化成 `uint8_t command`,identify/flush 能发;
4. `AHCIBlockDevice::block_count()` 报真值、`flush()` 下发真命令,`run-kernel-test` 705/0。

## 步骤

### 1. 构建 + 测试(真退出码)

```bash
git checkout 039_ahci_dma 2>/dev/null || git checkout 039_*
cmake -B build -S . && cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
cmake --build build --target run-kernel-test
```

**期望**:`build=0`;run-kernel-test 全绿 705/0(这一弧测试数不变,因为是重构 + identify/flush)。

### 2. DMA 收编

```bash
grep -nE 'g_dma_pool|PrdtBuilder' kernel/drivers/ahci/ahci.cpp
# 期望:cmd_list/fis 用 g_dma_pool.alloc(PAGE_SIZE);PrdtBuilder<MAX_PRDT_ENTRIES> 构 PRDT
grep -cE 'g_pmm\.alloc.*cmd|0xFFFFFFFF80000000' kernel/drivers/ahci/ahci.cpp
# 期望:0(散装 DMA 的硬编码偏移没了)
```

**思考**:为什么 command list 的 DmaBuffer 析构只 free 物理页、不 unmap?——见 037 的 GOTCHA #7:direct-map PTE 是永久对照表,unmap 它会拆永久槽 → demand paging 死循环。`DmaPool` 已把这条焊死。

### 3. IDENTIFY / FLUSH / block_count

```bash
grep -rn 'identify\|flush\|capacity_blocks_' kernel/drivers/ahci/
# 期望:ahci.hpp:108 identify(0xEC)、:114 flush(0xEA);ahci_block_device.cpp create 时 identify 填 capacity,flush() 调 ahci_->flush
```

关键的端到端验证:`AHCIBlockDevice::block_count() > 0` 在 run-kernel-test 里真机过——它证明 IDENTIFY 全链路(发命令 → DMA 读 1 sector → 解析 words 60-61 28-bit 容量)都对。

### 4.(思考)遗留的债

这一弧故意没做两件事:**中断驱动**(仍轮询 CI 位)和 **48-bit 容量**(仍 28-bit)。去看 `execute_command` 里的等待循环,你应该能看出它为什么在单核阻塞 IO 下够用、什么时候会成瓶颈(SMP + 高并发)。**记着债比假装没有强。**

## 验收清单

- [ ] 构建 `build=0`,run-kernel-test 705/0。
- [ ] `ahci.cpp` 用 `g_dma_pool` + `PrdtBuilder`,无散装 DMA 残留。
- [ ] `identify`/`flush` 能发,`block_count()` 报真值。
- [ ] 能说清「为什么 command list DmaBuffer 不 unmap」「为什么还轮询 CI(留什么债)」。

## 别做这些

- **别**给 command list / FIS 的 DmaBuffer 加 `vmm.unmap`——GOTCHA #7,QEMU 卡死。
- **别**假设 AHCI 容量 = img 文件大小(038 的 GOTCHA #8),以 IDENTIFY 报告为准。
