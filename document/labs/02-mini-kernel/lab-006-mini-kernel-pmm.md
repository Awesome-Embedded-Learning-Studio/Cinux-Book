---
title: Lab 006 · 物理内存管理
---

# Lab 006 · 物理内存管理

> 这个 lab 配套 [006 · 物理内存管理](../../book/02-mini-kernel/006-mini-kernel-pmm.md)。目标:用一张位图把 E820 变成可分配的 4KB 物理页,实现 `alloc_page`/`free_page`,并在 QEMU 内核测试里验它。**位图怎么布局、E820 怎么 carve、链接器符号怎么取**,都自己来,不给现成答案。

## 实验目标

- 写 `_KB/_MB/_GB` 字面量和对齐工具(`align_up/down`)。
- 实现位图分配器:一位一页(4KB),128KB 位图管 4GB;`set/clear/test/find_first_free` 原语。
- `init`:先全占用,再从 E820 type=1 区域 carve 可用页(滤低 1MB、页对齐),再保护内核和 bootloader 区。
- `alloc_page`(first-fit,0 表 OOM)/ `free_page`。
- QEMU 内核测试 `test_pmm` 验连续分配回收、计数、不重复分配、OOM。

## 前置条件

- 完成 [Lab 005](lab-005-mini-kernel-entry.md):内核有 kprintf、有 QEMU 内核测试框架、串口能输出。
- 理解 E820 内存图(BootInfo.mmap)、位图、链接器符号取值约定。

## 任务分解

分五块走。

第一块,字面量与工具。写 `memory_literals.h` 的 `_KB/_MB/_GB/_TB`(constexpr UDL)、`mm_defines.h` 的 `align_up/align_down/is_aligned`(位运算,要求 align 是 2 的幂)。这些是纯函数,可以顺手配 host 单测。

第二块,位图原语。`s_bitmap[BITMAP_SIZE]`(128KB 静态数组)、`set_bit/clear_bit/test_bit`(除 8 得字节、模 8 得位)、`find_first_free`(先扫 `!=0xFF` 的字节再逐位找 0 bit,找不到返 -1)。

第三块,init。先把位图全置 `0xFF`(默认全占用);遍历 `boot_info->mmap`,只处理 `type==1`,滤掉低 1MB(`LOW_MEMORY_BOUNDARY`)、页对齐、`mark_region_free`;记 `s_highest_page`;再标内核区(`kernel_phys_base` + 内核大小)和 bootloader 区(`0x0–0x10000`)used。

第四块,alloc/free。`alloc_page`:`find_first_free`→置位→返回 `page_idx*PAGE_SIZE`,找不到返 0。`free_page(phys)`:phys 为 0 直接忽略,否则算 page_idx、清位、计数。

第五块,内核测试。在 QEMU 测试里加 `test_pmm`:连续 alloc N 次核对 `free_page_count` 递减、free 后回升、两次 alloc 不重页、alloc 到 OOM 返 0。走 isa-debug-exit 退出。

## 接口约束

这些得自己保证对、lab 不给现成代码:

- 尺寸:`PAGE_SIZE=4_KB`、`MAX_MEMORY=4_GB`、`MAX_PAGES=1M`、`BITMAP_SIZE=128KB`、`LOW_MEMORY_BOUNDARY=1_MB`。
- 链接器符号:`extern char __kernel_size;`,取值用 `&__kernel_size`(不是 `__kernel_size`,后者读到的是字节不是大小)。
- E820:`type==1` 才可用;区域可能横跨低 1MB 边界,要截断不是整段丢;起止要页对齐。
- alloc 返 0 = OOM(0 在 bootloader 区已占用,不会真分配);`free_page(0)` 忽略。
- `mark_region_*` 要把跨页的 length 换算成页数,注意 `(phys+length+PAGE_SIZE-1)/PAGE_SIZE` 的向上取整。

## 验证步骤

QEMU 内核测试(主):

```bash
cmake --build build --target run-kernel-test
```

`test_pmm` 的断言全过、退出码 0。

量产内核看统计:

```bash
cmake --build build --target run
```

串口依次出现 `[MINI] PMM: kernel_phys=0x20000, kernel_size=0x... (... pages)`、`marking bootloader 0x0-0x10000 used`、`Total N pages (M MB), Free ... pages (... MB)`。Free 应是几百 MB 量级(QEMU 给 512MB),`kernel_size` 要和 `mini_kernel.bin` 文件大小对得上。

## 常见故障

PMM 报的内核大小明显不对(几百字节或一个单字节值)。`__kernel_size` 没加 `&`,读到了地址处的字节。改成 `&__kernel_size`。

alloc 出来的页落在 1MB 以下、一用就崩。低 1MB 没滤,E820 报告的低区被标成了 free。补 `LOW_MEMORY_BOUNDARY` 截断逻辑。

Free 统计是 0 或负数。init 算错了——要么 E820 全被滤掉、要么 `mark_region_free`/计数有 off-by-one。对照 `mini_kernel.bin` 大小和 QEMU 内存(512MB)核 `kernel_size` 和 `Total`。

位图访问越界(page_idx ≥ MAX_PAGES)。E820 某区域超出 4GB,或没做 `page < MAX_PAGES` 越界检查。`mark_region_*` 和 `free_page` 都加边界判断。

两次 alloc 拿到同一页。`alloc_page` 置位后没更新位图,或 `find_first_free` 逻辑错。核 `set_bit` 是否真写了 `s_bitmap`。

OOM 判错(还能分却返 0,或分光了还返非 0)。`find_first_free` 的扫描终止条件或 `s_free_pages` 计数不同步。单步看位图和计数。

## 通过标准

- `run-kernel-test` 里 `test_pmm` 全过、退出码 0。
- 量产 `make run` 串口出现 PMM 三行统计,Free 是合理正值,`kernel_size` 对得上 `mini_kernel.bin`。
- 全程没碰中断/IDT/PIC——那是 [Lab 007](lab-007-mini-kernel-intr.md);也没碰虚拟内存——VMM 是后面。
