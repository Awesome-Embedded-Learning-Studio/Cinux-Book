---
title: 03 · 代码路线:模型、init、链接器符号、alloc/free
---

# 代码路线:模型、init、链接器符号、alloc/free

## 1. 模型:一位一页,128KB 管 4GB

常量都集中在 [pmm.h](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/mm/pmm.h),而且用了 001 引入的内存字面量让它一目了然:

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

## 2. init:先全占用,再从 E820 carve,再保护内核

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

## 3. 链接器符号 &__kernel_size:为什么内核大小要问链接器

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

## 4. alloc/free:first-fit 与 0 哨兵

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
