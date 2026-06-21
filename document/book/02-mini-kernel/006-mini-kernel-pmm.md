---
title: 006 · 物理内存管理
---

# 006 · 物理内存管理:用位图吃下 E820

> [005](005-mini-kernel-entry.md) 的内核已经会说话,还把那张 E820 内存图整整齐齐 dump 到了串口上。可那只是"念出来",内核手里还是空的——它不知道哪块内存能用、哪块被占了,`operator new` 调一下还是原地 `hlt`。这一章,我们要给内核装一个**物理内存管理器(PMM)**:用一张位图,把 E820 报告的可用内存变成"能分配、能回收"的一个个 4KB 物理页。从这以后,内核才算真正开始管资源。

## 这一章我们要点亮什么

目标很具体:实现一个 `alloc_page()` / `free_page()` 的分配器。内核想知道"给我一页空闲物理内存",PMM 就从位图里找一个空位、把那页的物理地址交出来;用完了 `free_page` 还回去,位图上那一位清掉。

它的依据是 005 已经收集好的 E820 内存图。PMM 在初始化时遍历这张图,把 BIOS 报告"可用"(type=1)的区域标记成可分配,把内核自己、bootloader、以及低 1MB 那些不能碰的地方标记成占用。之后 `alloc_page` 就在这张位图上做最朴素的"找第一个空位"(first-fit)。

做完之后,`make run` 的串口上会多出几行 `[MINI] PMM:` 的统计——内核多大、占了多少页、总共多少页可用。而 `run-kernel-test` 里的 `test_pmm` 会真去连续分配、回收、核对计数,确认这个分配器在边界上没漏。

## 为什么现在需要它

一个没有内存管理的内核能干什么?老实说,干不了什么正经事。后面要写的几乎每一样——进程的内核栈、页表、文件缓存、用户进程的地址空间——底下都需要"给我一页物理内存"这个原语。没有 PMM,这些都是空中楼阁。

那为什么是位图?因为它是物理页分配器里最简单、最直白的一种。每一页(4KB)对应位图里一个 bit:1 表示占用、0 表示空闲。要找空闲页,就在位图里扫第一个 0;要回收,就把对应 bit 清掉。这种分配器不快(分配是线性扫描),不省(128KB 位图管 4GB),但它**正确性容易保证、行为容易理解**,对一个教学内核来说,正是"先把路修通"的第一步。后面真要讲究性能,再换成 buddy 之类的——但那是后话,这一章我们只要"能正确地分、正确地收"。

> 外部依据:OSDev 的 Physical Memory Management / Page Frame Allocation 页对比了位图、栈式、buddy 这几类分配器的取舍;位图法以"实现简单、回收 O(1)、分配需扫描"著称,常被教学内核采用。

## 设计图

先看位图这个数据结构怎么把物理内存"拍扁"成一张表:

```text
物理内存(每页 4KB)
  页0    页1    页2    页3   ...   页(4GB/4KB-1)= 1M 页
   │      │      │      │            │
   ▼      ▼      ▼      ▼            ▼
 bit0   bit1   bit2   bit3   ...   bit(1M-1)
  └──────────── 一位一页,装进 128KB 的 s_bitmap ────────────┘
   1=占用  0=空闲
```

`MAX_MEMORY = 4_GB`、`PAGE_SIZE = 4_KB`,所以最多 `MAX_PAGES = 1M` 页,位图 `BITMAP_SIZE = 1M/8 = 128KB`(每字节 8 页)。这个 128KB 的数组是 PMM 的全部状态,放在内核的 `.bss` 里。

再看初始化的思路。这里有个反直觉但很稳的设计:**先把所有页都标成"占用",再把能用的"挖"出来**。这比"先全空、再标占用"安全——默认拒绝,只对确认可用的开绿灯,漏标顶多是少分到内存,绝不会把保留区误分出去:

```text
init(boot_info):
  ① 位图全置 1(全占用)           ← 默认拒绝
  ② 遍历 E820:
       type==1(可用)的区域 →
         滤掉低 1MB、页对齐 → 标 free(挖出来)
  ③ 内核自身区域 → 标 used(保护)
  ④ bootloader 区 0x0–0x10000 → 标 used(保护)
```

alloc/free 则是在这张表上做最简单的位操作:

```text
alloc_page():  扫描位图找第一个 0 → 置 1 → 返回 该页物理地址(page_idx × 4KB)
               扫不到 → 返 0(OOM)
free_page(p):  p/4KB 得 page_idx → 该 bit 清 0
```

## 代码路线

### 1. 模型:一位一页,128KB 管 4GB

常量都集中在 [pmm.h](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/mm/pmm.h),而且用了 005 引入的内存字面量让它一目了然:

```cpp
constexpr uint64_t PAGE_SIZE            = 4_KB;      // 每页 4KB
constexpr uint64_t MAX_MEMORY           = 4_GB;      // 最多管 4GB
constexpr uint64_t MAX_PAGES            = MAX_MEMORY / PAGE_SIZE;   // 1M 页
constexpr uint64_t BITMAP_SIZE          = MAX_PAGES / 8;            // 128KB
constexpr uint64_t LOW_MEMORY_BOUNDARY  = 1_MB;      // 低 1MB 边界
```

`4_KB`、`4_GB` 这些是 [memory_literals.h](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/mm/memory_literals.h) 里 `constexpr` 的用户定义字面量,编译期就换成 `4096`、`0x100000000`。比起满屏 `0x1000`、`1073741824`,它们让"这一章到底在管多大的内存"一眼可读,还不会多花一个字节运行时开销。位图本身是个静态数组:`static uint8_t s_bitmap[BITMAP_SIZE]`,128KB,躺在内核 `.bss`。

位图的基本原语就是除以 8 得字节、模 8 得位内偏移:

```cpp
void set_bit(uint64_t index) {
    s_bitmap[index / 8] |= (1U << (index % 8));
}
```

`clear_bit`、`test_bit` 同理。找空闲页 `find_first_free` 有个小优化:先按字节扫,只对 `!= 0xFF` 的字节(说明里面有 0 bit)再逐位找,比一位一位扫快 8 倍。

### 2. init:先全占用,再从 E820 carve,再保护内核

[pmm.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/mm/pmm.cpp) 的 `init` 严格按设计图的四步走。第一步把位图全置 `0xFF`——所有页默认占用。

第二步是核心:遍历 E820,把可用区域挖出来。每个 E820 条目有 `base`、`length`、`type`,只有 `type == 1`(usable)才处理:

```cpp
for (uint32_t i = 0; i < info->mmap_count; i++) {
    const MemoryMapEntry* entry = &info->mmap[i];
    if (entry->type != 1) continue;          // 非可用区跳过

    uint64_t base = entry->base, length = entry->length;

    // 滤掉低 1MB(bootloader 和 BIOS 数据区在这)
    if (base < LOW_MEMORY_BOUNDARY) {
        if (length <= LOW_MEMORY_BOUNDARY - base) continue;   // 整段都在低 1MB,丢
        length -= (LOW_MEMORY_BOUNDARY - base);               // 部分重叠,截掉低的部分
        base = LOW_MEMORY_BOUNDARY;
    }

    // 页对齐后标 free
    uint64_t aligned_base = (base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    ...
    mark_region_free(aligned_base, aligned_length);
}
```

这里有两个容易想当然的地方。一是低 1MB 必须滤掉:那里挤着 MBR、bootloader、BIOS 数据区、还有我们 004 加载内核的区域,谁都不能动。而且 E820 报告的区域可能正好横跨 1MB 边界(比如从 `0xC0000` 延伸到 `0x120000`),所以不是简单"整段丢",而是要算出重叠部分、把 `base` 抬到 1MB 之上。二是页对齐:分配的最小单位是 4KB 页,区域起止如果不是页边界,得向上取整对齐,否则会分出"半页"。

第三、四步把内核自己和 bootloader 区域标回占用。这里需要知道"内核有多大"——这就引出下一个关键技术点。

### 3. 链接器符号 &__kernel_size:为什么内核大小要问链接器

内核运行时怎么知道自己镜像有多大?源码里有这么几行,看着别扭:

```cpp
extern "C" {
    extern char __kernel_size;      // 来自 linker.ld
    extern char __mini_kernel_end;
}
...
uint64_t kernel_size = reinterpret_cast<uint64_t>(&__kernel_size);
```

注意那个 `&__kernel_size`——我们要的是"内核大小"这个**数值**,但写的是"取这个符号的**地址**"。这不是笔误,是链接器符号的用法约定。

在 C 里,一个链接器脚本定义的符号(像 `__kernel_size = . - KERNEL_PHYS_BASE;` 这种),它的"值"就等于它在进程地址空间里的"地址"。而 C 声明 `extern char __kernel_size;` 把它当成一个 `char` 变量,那么 `&__kernel_size` 拿到的地址值,正好就是链接器给这个符号赋的那个数(在这里就是内核的字节数)。直接写 `__kernel_size`(不带 `&`)反而是错的——那会去读那个地址处的一个字节,得到的是内核镜像首字节,不是大小。

这个"符号即地址、取址即取值"的把戏,是内核开发里反复出现的模式:获取 `.bss` 起止、内核起止、各种段大小,全靠链接脚本打符号、C 里 `&symbol` 取值。它绕不开,但第一次写的人十有八九会写成不带 `&` 的版本,然后纳闷"为什么内核大小是 0x55 之类的怪值"。这正是这一章的一个经典坑(见调试现场)。

### 4. alloc/free:first-fit 与 0 哨兵

有了位图,分配就是 [pmm.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/mm/pmm.cpp) 里这么几行:

```cpp
uint64_t alloc_page() {
    int64_t page_idx = find_first_free();
    if (page_idx < 0) return 0;                 // OOM
    set_bit(page_idx);
    s_free_pages--;
    return page_idx * PAGE_SIZE;                // 物理地址
}

void free_page(uint64_t phys) {
    if (phys == 0) return;                      // 哨兵,忽略
    uint64_t page_idx = phys / PAGE_SIZE;
    if (page_idx >= MAX_PAGES) return;
    if (test_bit(page_idx)) { clear_bit(page_idx); s_free_pages++; }
}
```

分配是 first-fit:从位图低位往高位扫,第一个 0 bit 就拿走。简单,但有个特性——它会反复从同一端分配,导致低位页频繁进出、高位页积压。对教学内核无所谓,真要均匀分布可以记一个"上次分配到哪"的游标从那继续扫。

`0` 作为 OOM 的返回值,这里其实有个隐含约定:物理地址 `0` 在低 1MB、init 时已标占用,`find_first_free` 永远不会返回它,所以用 `0` 当"没有可用页"的哨兵是安全的——它不会和一个真实的分配结果混淆。`free_page` 收到 `0` 也直接忽略,配对一致。

## 调试现场

这一章的坑,一个是"链接器符号怎么取",一个是"哪些内存不能分"。

第一个就是上面讲的 `__kernel_size`。症状是 PMM 报告的内核大小明显不对(比如几 KB 内核报成几百字节,或一个莫名其妙的单字节值)。根因是写成 `__kernel_size`(不带 `&`),读到了那个地址处的字节而非符号值。修复就是 `&__kernel_size`。判断方法:量产内核串口上的 `[MINI] PMM: kernel_size=0x...` 那行,大小应该和 `mini_kernel.bin` 的文件大小一致——对不上就是符号取值错了。

第二个是忘了滤低 1MB。症状是 alloc 出来的页落在 `0x0–0x100000` 之间,一用就踩到 bootloader 或 BIOS 数据区,内核莫名其妙崩。根因是 init 没把低 1MB 标占用(或 E820 报告了低区可用但没滤)。修复就是那个 `LOW_MEMORY_BOUNDARY` 截断逻辑。判断方法:连续 alloc 几页,看返回地址是不是都 ≥ 1MB。

第三个(也是 005 末尾预告、006 notes 里专门记的)是对象库与全局构造。`pmm.cpp`、`format.cpp` 这些被编成静态库再链进内核,如果链接/构造链没接对,全局对象的构造不被调用。这一章的测试头 [kernel_test.h](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/test/kernel_test.h) 抽成公共头、`linker.ld` 用 `KEEP(*(.init_array))`,都是为了堵这条线。判断方法:`test_cpp_basic` 里"全局对象构造"那条过没过。

## 验证

这一章的测试第一次覆盖了"内核数据结构"本身,而不只是 C++ 运行时。

QEMU 内核测试是主战场:

```bash
cmake --build build --target run-kernel-test
```

`test_pmm` 会真刀真枪地验:连续 `alloc_page` 若干次、看 `free_page_count` 是不是按预期递减;把分配到的页 `free` 回去、看计数回升;确认两次分配不会拿到同一页;分配到 OOM 时拿到 `0`。这些断言跑在真内核里(它们依赖位图静态数组、E820 解析,host 测不了)。

量产内核看统计:

```bash
cmake --build build --target run   # 或 make run
```

串口上会看到 `[MINI] PMM: kernel_phys=0x20000, kernel_size=0x... (... pages)`、`marking bootloader 0x0-0x10000 used`、最后一句 `Total N pages (M MB), Free ... pages (... MB)`。那个 Free 数字就是内核此刻能动用的物理内存总量——它得是个合理的正值(比如 QEMU 给 512MB,Free 应该是几百 MB 量级),是 0 或负就是 init 算错了。

## 下一站

内核现在能分配物理页了。可注意:它分到的是**物理地址**,而我们身处 64 位长模式、地址翻译走页表——一个裸的物理地址没法直接当指针用(除非正好在恒等映射的那 8MB 里)。要把"物理页"变成"内核能随便用的虚拟地址",我们需要一层虚拟内存管理(VMM),建内核自己的页表、做物理↔虚拟的映射。

不过在那之前,还有一件更基础的事得先办:内核现在 `cli; hlt` 完全不响应中断,任何异步事件(定时器、键盘)它都接不住。下一章 [007 · 中断](007-mini-kernel-intr.md),我们给 mini kernel 装上 IDT 和 PIC,让它第一次能"被打断"并做出响应。中断和内存是内核的两条腿,这一章迈出了内存这条,下一章迈另一条。

---

### 参考

- OSDev — [Physical Memory Management](https://wiki.osdev.org/Physical_Memory_Management)(位图 vs 栈式 vs buddy 分配器取舍)、[Page Frame Allocation](https://wiki.osdev.org/Page_Frame_Allocation)、[Detecting Memory (x86): E820](https://wiki.osdev.org/Detecting_Memory_(x86)#e820)。
- ld 链接脚本 — 符号即地址、`&symbol` 取值的约定(OSDev [Linker Scripts](https://wiki.osdev.org/Linker_Scripts) 与 ld 手册 Symbol 一节)。
- Linux(伙伴系统)/ xv6(`kalloc`/页表)仅作"更高级分配器与虚拟内存"的对比参照,非本 tag 实现。
- 本 tag 源码:[pmm.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/mm/pmm.cpp)/[pmm.h](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/mm/pmm.h)、[mm_defines.h](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/mm/mm_defines.h)、[memory_literals.h](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/mm/memory_literals.h)、[test_pmm.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/test/test_pmm.cpp)、[linker.ld](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/linker.ld)。
- 调试素材提炼自 [006-01-linker-symbol-access.md](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/document/notes/006/006-01-linker-symbol-access.md)、[006-02-object-library-global-ctors-not-called.md](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/document/notes/006/006-02-object-library-global-ctors-not-called.md)。

> 参考 URL 的有效性会在全局审查阶段用 open-websearch(bing)统一核活。
