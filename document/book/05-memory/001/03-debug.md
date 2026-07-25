---
title: 03 · 调试现场与收尾
---

# 调试现场与收尾

## 调试现场

这一章没有 notes 文件,但 PMM 有几个写错就「静默错乱」的隐患,值得当成调试现场点出来——它们都不报错,只会让分配器慢慢坏掉,极难查。

一是 **bitmap 忘了把自己标占用**——这大概是最经典的一个。init 的 Step 7 漏掉,后果是 `alloc_page` 早晚会把 bitmap 自身所在的物理页当成空闲页分配出去。拿到这「页」的代码往里写数据,实际写的是 bitmap——某一位被翻转,某段内存的占用状态就错了。症状是极其诡异的「分配器随机出错」「内存随机损坏」,而且和具体的写入时机有关,复现困难。根因好查也好防:Step 7 把 bitmap 自己的物理范围 `mark_region_used`。这类「分配器忘了保护自己的元数据」的 bug 是分配器实现里最常见的,所以 init 里那一串「先全占用、再 carve、再把元数据标回占用」的仪式,一步都不能省。

二是 **分配出去的页写进去,屏幕花了或机器重启**。这是 `parse_memory_map` 的过滤漏了一道。最常见是 type 过滤没做,把 type-2 的保留段(里面可能有 framebuffer MMIO、ACPI 表)当成可用 RAM 发了出去;写这种「页」实际是写硬件寄存器,轻则花屏,重则踩坏 ACPI 导致重启。或者 <1MB 边界没丢,发出去了 BIOS 区。排查方向:看分配到的物理地址落在哪——如果它落在一个已知的保留区(比如 framebuffer 的 `fb_addr` 附近),那就是 parse 没把它排除掉。这三道过滤(type、1MB、4KB 对齐)是 PMM 安全的第一道闸,漏一道就可能在分配器里埋雷。

三是 **地址换算整体错位**。bitmap 第 N 位对应物理地址 `N × 4KB`,这个对应关系一错,全乱。常见的错法是 `KERNEL_VMA` 用错——`bm_virt - KERNEL_VMA` 算 bitmap 的物理地址时,如果 `KERNEL_VMA` 和链接脚本/页表里实际用的高半区偏移不一致,算出来的就是错的物理地址,Step 6/7 标占用的就是错的地方(该保护的没保护、不该保护的占了)。这种 bug 的症状是「明明保护了 kernel,却还是分配到了 kernel 的页」。对策:把 `KERNEL_VMA` 和链接脚本里的 `KERNEL_VMA`、和页表实际映射的偏移,三者对齐核一遍。

四是 **「OOM 返回 0 却没人检查」**。`alloc_page` OOM 返回 0,这是契约。但如果有调用者忘了检查、直接拿返回值当地址用,OOM 时就写物理地址 0(BIOS 区)。这在 boot 期内存充足时不发作,等哪天内存紧张了才冒出来。养成习惯:每次 `alloc_page`/`alloc_pages` 之后,第一件事是 `if (!addr) { 报错 }`。host 单测里有专门的 `OOM returns 0` 用例,就是在固化这个契约。

## 验证

PMM 是纯逻辑组件,大部分行为可以在 host 上用单测镜像出来,不依赖真硬件。`parse_memory_map` 的过滤逻辑、bitmap 的分配/释放/连续/计数,都能脱离 E820 真值来测。[test_pmm.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_pmm.cpp) 把这些逻辑镜像了一份,用 `-O2` 编、`CINUX_HOST_TEST` 门控:

```cpp
TEST("parse_memory_map: filters non-usable types") { ... }      // type-2 被丢
TEST("parse_memory_map: clips partial overlap with low 1MB") { ... }  // 1MB 截断
TEST("pmm: alloc_page returns page-aligned address") { ... }    // 4KB 对齐
TEST("pmm: OOM returns 0") { ... }
TEST("pmm: double free is a no-op") { ... }                     // free 不虚增计数
TEST("pmm: alloc_pages fails on fragmented memory") { ... }     // 碎片时连续分配失败
```

这些把 parse 的三道过滤、分配的对齐和 OOM、释放的 no-op 防御、连续分配的碎片处理,都焊了一遍。注意它是镜像测法——内核代码在 host 上跑不起来(用了链接器符号、BootInfo),所以把 bitmap 算法和 parse 逻辑抄一份到测试里测。跑它们:

```bash
ctest --test-dir build -R pmm --output-on-failure
```

但「bitmap 真放对了位置、E820 真解析对了、统计真合理」这些只有 QEMU 里对着真 E820 才验得了真。所以还有一组机内测 [test_pmm.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_pmm.cpp),在 QEMU 里跑 bootloader 采集的真内存图:

```cpp
void test_init_and_stats() {            // init 后 total/free 合理
void test_alloc_free_cycle() { ... }    // alloc 再 free, 计数守恒
void test_bulk_alloc_free() { ... }     // 批量循环, free_page_count 回到初值
void test_alloc_pages_contiguous() { ... }  // 连续分配返回的地址真的连续
void test_free_zero_noop() { ... }      // free(0) 不乱
void test_double_free_noop() { ... }    // double-free 不乱
```

其中 `test_bulk_alloc_free` 最有价值:它反复分配再全部释放,验证 `free_page_count` 能精确回到初始值——这是「没有泄漏、没有 double-free 计数虚增」的最直接证明。跑它:

```bash
cmake --build build --target run-big-kernel-test
```

init 完成时还会打印一行 `[PMM] Total: XuMB, Free: YuMB`,看这行数字合理(QEMU 默认 128MB 或你配置的内存量,free 略小于 total,差的是 kernel+bitmap),就说明账本建对了。

## 下一站

到这里,内核第一次有了「物理内存账本」。`alloc_page` / `free_page` 能用,bitmap 准确记录每一页的状态,OOM 和 double-free 都有防御。后面所有要内存的子系统,都可以向 PMM 要页了。

但你会发现一个明显的缺口:我们只能分配**物理**页,却没法把它们挂到**虚拟**地址空间里。现在内核访问内存,用的还是 bootloader 搭的那套固定页表;想给一个新进程做独立的地址空间、想把某个物理页映射到不同的虚拟地址,都没有机制。013 那会儿为了点亮 framebuffer,我们临时写了那个硬编码页表地址的 `map_mmio`——那只是个 hack,不是正经的虚拟内存管理。

下一站,我们就把这件事做正经:一个虚拟内存管理器(VMM),能把 PMM 分配的物理页,按需映射到虚拟地址,管理内核(乃至将来进程)的地址空间。那才是内存子系统的第二块、也是最核心的一块拼图。不过那是下一章的事,我们先确认这本物理账本是结实的。

---

### 参考

- OSDev — [Detecting Memory (x86) / E820](https://wiki.osdev.org/Detecting_Memory_(x86)):BIOS `INT 0x15 AX=0xE820` 内存图调用、entry 的 type 值(1=usable、2=reserved、3=ACPI reclaimable、4=ACPI NVS)。本章 `parse_memory_map` 的过滤以此为准。
- OSDev — [Page Frame Allocation](https://wiki.osdev.org/Page_Frame_Allocation):bitmap/位图分配器「一页一位、扫描找空闲位」的基本设计,以及它的取舍(bitmap 简单但连续分配慢,适合 boot 期)。本章 PMM 的形态与此一致。
- GCC 在线文档 — [`__builtin_ctzll`](https://gcc.gnu.org/onlinedocs/gcc/Other-Builtins.html):返回 `unsigned long long` 中最低位 set bit 的索引(映射到 `BSF` 指令),用于本章 64 位组扫描加速。
- 本 tag 源码:[pmm.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/pmm.hpp) / [pmm.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/pmm.cpp)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp)(Step 7 `g_pmm.init`)、[boot_info.h](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/boot_info.h)(`MemoryMapEntry` 24 字节、`mmap[32]`、`kernel_phys_base`);测试 [test_pmm.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_pmm.cpp)(host 镜像)、[test_pmm.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_pmm.cpp)(QEMU 真 E820)。
