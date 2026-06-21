---
title: Lab 015 · 给内核一本物理内存账本:bitmap PMM
---

# Lab 015 · 给内核一本物理内存账本:bitmap PMM

> 配套章节:[015 · 给物理内存建账本:bitmap 物理内存管理器](../../book/05-memory/015-mm-pmm.md)。这一关给你目标和约束,不贴答案——bitmap 放哪、init 怎么 carve,都得你自己想清楚。

## 实验目标

给内核一本物理内存的「账本」,让它第一次能动态分配、回收物理页。拆成几个能独立验证的子目标:

1. 能解析 E820 内存图:从 bootloader 给的 `BootInfo.mmap[]` 里提取出真正可用的物理内存段。
2. 能建起 bitmap:按最高物理地址确定 bitmap 大小,每 4KB 物理页对应一位。
3. init 正确:把可用段标成空闲,同时保护 kernel image、栈、bitmap 自身不被分配出去。
4. 能分配与回收:`alloc_page` / `free_page`,还要支持连续多页 `alloc_pages`,OOM 返回 0,double-free 安全 no-op。

做完这四条,内核就有了内存管理的地基,后面 VMM、堆、进程都踩在它上面。

## 前置条件

你得先过 Lab 014(内核稳定运行、中断能跑)。这一关依赖 bootloader 已经把 E820 内存图采好、填进 `BootInfo.mmap[]`(物理 0x7000),`mmap_count` 有效、`kernel_phys_base` 记录了内核加载位置。E820 的采集不是这关的活,假定它就位。

另外要熟悉链接脚本提供的符号:`__kernel_stack_top`(栈顶虚拟地址)这一关要用,bitmap 就放在它后面。

## 任务分解

**第一步:解析 E820。** 遍历 `BootInfo.mmap[]`,只收 type 为「可用 RAM」的段。每段做三道处理:整段在 1MB 以下的全丢;跨 1MB 边界的,截掉 1MB 以下那截,base 抬到 1MB;再把 base 向上、length 向下都按 4KB 对齐(只留整页),对齐后不足一页的整段丢。这三道一道都不能少——不过滤 type 会把保留区发出去踩硬件,不丢 1MB 以下会动 BIOS,不对齐会让「位 ↔ 地址」换算错位。

**第二步:确定 bitmap 大小并放置。** 找所有 usable 段里的最高物理地址,`highest_page = 最高地址 / 4KB`,`bitmap 字节数 = (highest_page + 7) / 8`。然后解决一个鸡生蛋的问题:bitmap 自己放哪?得放在一个「此刻已经映射、能访问」的地方——放在栈顶符号之后、页对齐的位置是个务实选择。想清楚为什么不能随便放个虚拟地址(会 page fault,而此刻 PMM 还没起来)。

**第三步:init:先全占用,再 carve 可用。** 这里有个安全取向要想明白。先用 0xFF 把整个 bitmap 填成「全占用」,再把 usable 段对应的位清成 0(标可用)。想清楚为什么是「全占用再 carve 可用」而不是「全可用再标占用」——E820 只告诉你哪些可用,不告诉你所有不可用的地方,反过来做更安全。然后别忘了把已经占用的东西重新标回占用:kernel image + 栈(从 `kernel_phys_base` 到 bitmap 起点的物理范围)、以及 bitmap 自己。这两步漏一个都会埋雷。

**第四步:分配与释放。** 单页分配:在 bitmap 里找第一个 0 位、置 1、返回 `位号 × 4KB` 作为物理地址;找不到返回 0(OOM)。建议用 64 位一组地扫(一次看 64 页),配合「count trailing zeros」类的位运算找最低位的 0,比一位位试快得多;尾部不足 8 字节的零头单独逐位兜底。连续多页分配:线性扫,找连续 N 个 0;单页走快路径别付线性扫描的代价。释放要对非法输入 no-op:释放 0(哨兵不是真页)、越界地址、double-free 都静默返回,别让计数虚增。

## 接口约束

你要实现出来的东西,对外长这样(职责和签名,不给实现):

- `parse_memory_map(const BootInfo&, MemoryRegion* regions, uint32_t max) -> uint32_t`:从 E820 提取 usable 段,返回段数。
- `PMM::init(const BootInfo&)`:建 bitmap、初始化占用状态、打印统计。
- `PMM::alloc_page() -> uint64_t`:分配一页,返回物理地址;OOM 返回 0。
- `PMM::free_page(uint64_t phys)`:释放一页;0/越界/已空闲均 no-op。
- `PMM::alloc_pages(uint64_t count) -> uint64_t`:分配连续 count 页,返回基地址;失败返回 0。
- `PMM::free_pages(uint64_t phys, uint64_t count)`、`PMM::free_page_count() / total_page_count()`。

bitmap 内部常数(4KB 页、1MB 边界、高半区偏移 `KERNEL_VMA`)得你自己定,并和链接脚本、页表的实际偏移对齐。

## 验证步骤

纯逻辑(parse 过滤、bitmap 分配/释放、计数、连续、OOM、double-free)在 host 上镜像着测,不依赖真 E820、不依赖 QEMU。把 parse 和 bitmap 逻辑抄一份到测试里,用 `-O2` 编、`CINUX_HOST_TEST` 门控:

```bash
ctest --test-dir build -R pmm --output-on-failure
```

建议覆盖:type-2 被丢、1MB 以下被丢/截断、4KB 对齐、alloc 返回页对齐地址、alloc/free 循环计数守恒、OOM 返回 0、free(0) 和 double-free 是 no-op、连续分配成功、碎片时连续分配失败。

真 E820、bitmap 真放对位置、统计真合理,得在 QEMU 里对着 bootloader 采的内存图验。机内测跑 init 统计、批量 alloc/free 循环(`free_page_count` 精确回到初值,证明没泄漏没虚增)、连续分配地址真连续:

```bash
cmake --build build --target run-big-kernel-test
```

init 完成会打印 `[PMM] Total: XuMB, Free: YuMB`,看这行数字合理(QEMU 配置的内存量,free 略小于 total)就说明账本建对了。

## 常见故障

- **分配器随机错乱、内存随机损坏**:init 漏了把 bitmap 自身标占用,`alloc_page` 把 bitmap 的页发出去了,写「分配到的页」实际写的是 bitmap。务必把 bitmap 物理范围标占用。
- **分配到的页写进去,屏幕花了/机器重启**:`parse_memory_map` 的 type 过滤漏了,把保留段(含 framebuffer MMIO、ACPI)当可用发出去了。看分配到的地址是否落在已知保留区。
- **地址换算整体错位**:`KERNEL_VMA` 和链接脚本/页表实际的高半区偏移不一致,Step 里标占用的物理范围算错。三者对齐核一遍。
- **`free_page_count` 不守恒**:double-free 没挡,计数虚增;或 alloc 时漏减、free 时漏加。释放接口对非法输入必须 no-op。
- **OOM 时崩在调用方**:`alloc_page` 返回 0 没人检查,直接当地址写。每次 alloc 后第一件事检查 0。
- **bitmap 放的位置 page fault**:放到了没映射的虚拟地址。放在栈顶之后、已映射区域里。
- **连续分配总失败但内存明明够**:线性扫描的逻辑在遇到占用位时没把「连续计数」清零重新数,或单页没走快路径导致超时(后者不是 bug,是慢)。

## 通过标准

1. host 单测全绿:E820 三道过滤、alloc 页对齐、alloc/free 循环计数守恒、OOM 返回 0、free(0)/double-free 是 no-op、连续分配与碎片失败都对。
2. QEMU 机内测通过:init 统计合理、批量 alloc/free 后 `free_page_count` 精确回到初值、连续分配返回真连续地址、no-op 行为安全。
3. `[PMM] Total/Free` 那行数字和 QEMU 内存配置吻合。
4. init 仪式完整:全占用 → carve 可用 → 把 kernel image/栈/bitmap 自身标回占用,一步不漏。

做到这四条,内核就有了第一本物理内存账本,`alloc_page`/`free_page` 可用。但你还只能分配物理页,没法把它们挂进虚拟地址空间——那就是下一关虚拟内存管理器的活了。
