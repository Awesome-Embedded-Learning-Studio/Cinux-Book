---
title: 015 · 给物理内存建账本:bitmap 物理内存管理器
---

# 015 · 给物理内存建账本:bitmap 物理内存管理器

> 到 014 为止,内核有了输入(键盘)、有了输出(屏幕 + 串口),中断体系也开始真正干活了。但有个最根本的东西一直没动:内存。到目前为止,内核用的每一字节内存,要么是 bootloader 给它加载到的固定位置,要么是栈——它从来没「分配」过一页内存。想给将来的进程分地址空间、想做个堆、想在文件系统里缓存一个块,全都得先有一个能回答「给我一页物理内存」「这页我还你」的机制。这一章,我们就给物理内存建一本账本:用一个 bitmap 记录每个 4KB 物理页的占用状态,然后提供分配和回收的接口。这是整个内存子系统的第一块基石。

## 这一章我们要点亮什么

一件核心的事:内核第一次能**动态分配和释放物理内存**。调一个 `alloc_page()`,拿回一个 4KB 物理页的物理地址;用完了 `free_page(addr)` 还回去。`alloc_pages(count)` 还能一次要连续的若干页。

这件事的意义不只是「多了一个函数」。它意味着内核从「一切内存写死」变成了「内存可管理」。在此之前,我们能用的内存就是镜像和栈那固定的一块;从这一章起,内核有了一个可以按需取用的物理页池子。后面几乎所有子系统都要踩在它上面:虚拟内存管理(VMM)要把物理页映射成虚拟页,堆分配器要在页上切块,进程要把页挂进自己的地址空间。PMM 是它们共同的底下那一层。

这本「账本」的具体形态是一个 **bitmap(位图)**:物理内存按 4KB 一页划分,每一页对应 bitmap 里的一位,1 表示占用、0 表示空闲。分配就是「在 bitmap 里找一个 0,置 1」;释放就是「把那一位置 0」。听起来朴素,但bitmap 是物理页分配器最直接、也最容易实现正确的形态——我们这一章就用它。

## 为什么现在需要它

先说为什么是现在。014 之后,内核的外设已经相当齐全了:能显示、能收键盘、中断能跑。可你仔细想会发现,这些全是在「固定内存」上搭起来的——frame buffer 是 bootloader 映射好的显存,栈是链接脚本指定位置的,kprintf 的 sink 表是个静态全局变量。没有任何一处是「运行时按需要来的内存」。这条路走到进程就撞墙了:你没法给一个新进程分配页表、没法给它分独立的栈,因为你根本拿不到空闲物理页。所以在动进程之前,必须先把物理内存管起来。

再往远看一步,PMM 是后面整条内存线的地基。下一章(016)就是虚拟内存管理,而 VMM 干的事——把物理页映射到虚拟地址——天然需要先有一个「物理页从哪来」的来源,那正是 PMM。再往后的堆、进程地址空间,无一不是「向 PMM 要页、再在虚拟层面组织」。所以这一章看似只是写了个分配器,实际上它在给后面四五章铺最底下那层路。早做、做对,后面省一大堆事。

还有一个工程上的理由:PMM 是个**几乎纯逻辑**的组件(就是 bitmap 那一套位运算),几乎不碰硬件细节,极其适合用 host 单测把它焊死。这种「容易测对」的基础组件放在链路最底层,稳了,上层才好排查问题。

## 设计图

PMM 的工作分三个阶段:先搞清楚物理内存长什么样,再为它建 bitmap,最后用 bitmap 做分配。

```text
   BIOS E820 内存图 (bootloader 采集, 塞进 BootInfo.mmap[])
   ┌──────────────────────────────────────────┐
   │ base=0x00000000 len=0x9FC00  type=1 usable │  ← 640KB 常规内存
   │ base=0x0009FC00 len=...      type=2 reserved│  ← BIOS/保留
   │ base=0x00100000 len=0x3F...  type=1 usable │  ← 1MB 以上的可用 RAM
   │ base=0x...        type=2/3/4 ...           │  ← ACPI、MMIO 等
   └───────────────────┬──────────────────────┘
                       ▼ parse_memory_map
   过滤 type=1 + 丢弃 <1MB + 4KB 对齐
   ┌──────────────────────────────────────────┐
   │ usable regions[]: [1MB, ...], [...]        │
   └───────────────────┬──────────────────────┘
                       ▼ PMM::init
   ① 按最高物理地址算 bitmap 大小: 1 bit / 4KB 页
   ② bitmap 放在 __kernel_stack_top 之后 (已映射区域, 页对齐)
   ③ 先全置 1 (全占用), 再把 usable region 的位清 0 (标 free)
   ④ 把 kernel image + stack + bitmap 自身的位重新置 1 (保护)
   ┌──────────────────────────────────────────┐
   │  bitmap:  bit=1 占用, bit=0 空闲           │
   │           第 N 位 ↔ 物理地址 N*4KB          │
   └───────────────────┬──────────────────────┘
                       ▼ alloc_page / free_page
        alloc: find_first_free (64 位 ctzll 扫描) → 置 1 → 返回地址
        free:   对应位置 0  (0/越界/已空闲 → no-op)
```

bitmap 的核心不变量很简洁:**第 N 位,对应物理地址 `N × 4KB`**。整个 PMM 的正确性都建立在这个对应关系上。

## 代码路线

### 先搞清有哪些内存可用:解析 E820

物理内存不是一整块连续可用 RAM——BIOS 会告诉我们一张「内存图」,标出哪段可用、哪段是保留的(给 BIOS、ACPI、MMIO 用)。这张图通过 BIOS 的 E820 调用得到,bootloader 在实模式采集好,塞进 `BootInfo.mmap[]` 传给内核。每一项长这样([boot_info.h](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/boot_info.h)):

```c
typedef struct {
    uint64_t base;     // 这段的物理起始地址
    uint64_t length;   // 长度
    uint32_t type;     // 1=usable, 2=reserved, 3=ACPI reclaimable, 4=ACPI NVS ...
    uint32_t acpi;     // ACPI 扩展属性(通常 0)
} __attribute__((packed)) MemoryMapEntry;   // 24 字节, 和 E820 原始格式一致
```

PMM 第一步要做的,就是从这张原始图里,提取出「真正能分配给人家用」的那些段。这是 [pmm.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/pmm.cpp) 里 `parse_memory_map` 的活:

```cpp
for (uint32_t i = 0; i < info.mmap_count && count < max_regions; i++) {
    const auto& entry = info.mmap[i];
    if (entry.type != 1) continue;            // 只要 type-1 可用 RAM

    uint64_t base = entry.base, length = entry.length;

    // 丢弃 1MB 以下: 那里是 BIOS、实模式结构、我们自己的加载区, 不动
    if (base < LOW_MEM_BOUNDARY) {
        if (base + length <= LOW_MEM_BOUNDARY) continue;   // 整段都在 1MB 以下, 全丢
        length -= LOW_MEM_BOUNDARY - base;                  // 截掉 1MB 以下那截
        base = LOW_MEM_BOUNDARY;
    }

    // base 向上对齐到 4KB, length 向下对齐到 4KB(只留整页)
    uint64_t aligned_base = (base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    length -= (aligned_base - base);
    length &= ~(PAGE_SIZE - 1);

    if (length < PAGE_SIZE) continue;         // 对齐后不足一页, 丢
    regions[count++] = {aligned_base, length};
}
```

这里有三道过滤,每一道都有道理。**type 过滤**:只收 type-1,因为 type-2/3/4 那些是 BIOS、ACPI、硬件保留的,分配出去会踩坏硬件(比如把 framebuffer 的 MMIO 区域分给人,屏幕就花了)。**1MB 边界**:1MB 以下是 BIOS 数据区、实模式结构、bootloader 的加载区,全是历史包袱,内核不应该往里伸手,所以一律从 1MB(`0x100000`)起算。**4KB 对齐**:PMM 以 4KB 页为单位,段的起始和长度都得是页的整数倍,不然「第 N 位 ↔ N×4KB」的对应就对不齐。`base` 向上对齐(可能丢掉头部零头)、`length` 向下对齐(可能丢掉尾部零头),对齐后不足一页的整段丢弃。

这三道过滤少任何一道都会出事:type 不过滤会把保留区发给人家;<1MB 不丢会动 BIOS;对齐不做出「位↔地址」的换算就错位。它们是 PMM 正确性的第一道闸。

### bitmap 放哪:自举的鸡生蛋问题

拿到 usable region 之后,要为整个物理地址空间建 bitmap。bitmap 多大?看最高物理地址:`highest_page_ = max_addr / PAGE_SIZE`,`bitmap_size_ = (highest_page_ + 7) / 8`——最高物理地址对应的页号,除以 8(每字节 8 位),就是 bitmap 字节数。比如最高物理地址 4GB,就是 4GB/4KB = 1M 页,bitmap = 128KB,不大。

接下来是个微妙的「鸡生蛋」问题:**bitmap 自己也是一块内存,它该放在哪?** 你要管理内存,得先有一块内存放管理结构;但「分配内存」这件事还没就绪。这就是自举(bootstrap)的经典困境。

Cinux 的解法很务实:把 bitmap 放在 **kernel 栈顶(`__kernel_stack_top`)之后、页对齐**的地方。为什么放这里?因为这块区域是 bootloader/链接脚本早就映射好了的、内核此刻就能访问的——PMM 还没就绪时,只有这种「已映射」的位置是安全的。如果随手把 bitmap 放到一个没映射的虚拟地址,`init` 里第一个写 bitmap 的动作就 page fault 了,而此刻 PMM 自己还没起来,连崩都没法好好崩。

```cpp
uintptr_t stack_top_virt = reinterpret_cast<uintptr_t>(&__kernel_stack_top);
uintptr_t bm_virt = (stack_top_virt + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);  // 页对齐
bitmap_ = reinterpret_cast<uint8_t*>(bm_virt);
```

这里出现了一个关键的常数 `KERNEL_VMA = 0xFFFFFFFF80000000`——内核跑在「高半区」(higher-half),虚拟地址和物理地址之间差这个偏移。bitmap 放在虚拟地址 `bm_virt`,它对应的物理地址就是 `bm_virt - KERNEL_VMA`。这个换算在 `init` 后面会用到(要把 bitmap 自身标占用,得知道它的物理地址)。`KERNEL_VMA` 是个写死的约定,它和 013 里 `map_mmio` 那两个写死的页表地址是同一类东西——都是「bootloader/链接脚本布局约定」的硬编码,脆弱但在 boot 期够用。

### init 的反向思路:先全占用,再 carve 出可用

bitmap 放好了,怎么初始化它的内容?这里的思路有点反直觉,但很安全。看 [pmm.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/pmm.cpp) 的 `PMM::init`:

```cpp
// Step 4: 先把整个 bitmap 全置 1 —— 默认所有页都"占用"
for (uint64_t i = 0; i < bitmap_size_; i++) bitmap_[i] = 0xFF;
free_pages_ = 0;

// Step 5: 再把 usable region 里的位清 0 —— 只有这些才"可用"
for (uint32_t i = 0; i < region_count; i++)
    mark_region_free(regions[i].base, regions[i].length);

// Step 6: 把 kernel image + stack 重新标占用(它们在 usable region 里, 但不能发)
uint64_t used_phys_start = info.kernel_phys_base;
uint64_t used_phys_end   = bm_virt - KERNEL_VMA;
mark_region_used(used_phys_start, used_phys_end - used_phys_start);

// Step 7: 把 bitmap 自身标占用(同理, 不能把自己发出去)
uint64_t bm_phys = bm_virt - KERNEL_VMA;
mark_region_used(bm_phys, /* bitmap 占的页 */);
```

注意这个顺序:**先全部标占用,再 carve 出可用**。为什么反过来,不是「先全 free,再标占用」?因为 E820 只告诉你「哪些可用」,并不详尽地告诉你「哪些不可用」。可用的段(type-1)我们清楚,但保留段(type-2/3/4)可能漏报、可能有间隙我们没记。如果用「全 free 再标占用」的思路,任何一段我们没显式标占用的地方都会被当成可用,可能把保留区发出去。「全占用再 carve 可用」反过来:**默认一切不可用,只有 E820 明确说可用的那段才放开**。这样即使 E820 有遗漏,漏掉的地方也保持「占用」状态(只是浪费,不会出错),安全得多。这是个很值得记的设计取向:对内存这种「分错就踩硬件」的东西,宁可保守也别激进。

Step 6 和 Step 7 是两个容易漏、漏了就翻车的点。kernel image(代码、数据)、栈,它们物理上确实落在某个 usable region 里(E820 觉得「这是 RAM,可用」),但它们此刻正被内核用着,绝不能分配出去。bitmap 自己也一样——它就放在栈顶后面,也是 usable RAM,但它是分配器自己的账本,发出去等于自毁。所以 init 显式地「把这些我们已经占的位置重新标占用」。漏掉 Step 7(bitmap 自身),`alloc_page` 早晚会把 bitmap 自己的页发给人家,接下来你清零一个「分配到的页」,实际清的是 bitmap,分配器当场错乱——调试现场会专门讲这个。

### 分配与释放:64 位 ctzll 加速 + 连续分配

账本建好,分配就两件事:找一个 0 位、置成 1。释放反过来。单页分配 `alloc_page` 的亮点是它用 64 位一组地扫描,而不是一位一位试:

```cpp
uint64_t PMM::alloc_page() {
    int64_t idx = bm_find_first_free(bitmap_, highest_page_, bitmap_size_);
    if (idx < 0) return 0;              // 没找到 → OOM, 返回 0
    bm_set(bitmap_, idx);
    free_pages_--;
    return idx * PAGE_SIZE;             // 位号换算回物理地址
}
```

`bm_find_first_free` 的扫描是这么加速的:

```cpp
const auto* bm64 = reinterpret_cast<const uint64_t*>(bm);   // 把 bitmap 当 uint64 数组
for (uint64_t i = 0; i < qword_count; i++) {
    if (bm64[i] != ~0ULL) {                 // 这 64 位里至少有一个 0(有空闲页)
        int bit = __builtin_ctzll(~bm64[i]);  // 找最低位的 0
        return i * 64 + bit;
    }
}
// 尾部不足 8 字节的部分单独逐位扫
```

一次判断 64 页,而不是 64 次。`~bm64[i]` 把「占用图」按位取反成「空闲图」,`__builtin_ctzll`(count trailing zeros)直接给出最低位那个 1 的位置——也就是原 bitmap 里最低位的 0、即最低号的空闲页。`__builtin_ctzll` 是编译器内联的,通常映射到单条 `BSF` 指令,极快。这样扫描一段全满的 bitmap 是 O(字数)而不是 O(位数),对大内存差别明显。尾部那些不足 8 字节的零头,因为 `qword` 整除扫不到,单独用一个逐位循环兜底,保证 bitmap 大小不是 8 的倍数时也不漏。

注意 OOM 时 `alloc_page` 返回 0。用 0 当「分配失败」的哨兵安全吗?安全——因为第 0 页(物理地址 0)在 1MB 以下,早就被 `parse_memory_map` 过滤掉了,它永远不会是合法的分配结果,所以拿 0 当失败标志没有歧义。调用者拿到 0 就知道「没内存了」,该报错报错。不过这要求**所有调用者都得记得检查 0**——忘检查就用返回值当地址,OOM 时就会写物理地址 0,又踩 BIOS 区。这种「哨兵值约定 + 调用者必须检查」的契约,是 PMM 对外的接口规矩。

连续多页分配 `alloc_pages(count)` 就没这么潇洒了,它得线性扫每一位,找一段连续的 `count` 个 0:

```cpp
uint64_t PMM::alloc_pages(uint64_t count) {
    if (count == 1) return alloc_page();   // 单页走快路径
    uint64_t run = 0, start = 0;
    for (uint64_t p = 0; p < highest_page_; p++) {
        if (!bm_test(bitmap_, p)) {        // 这位空闲
            if (run == 0) start = p;
            if (++run >= count) {          // 凑够连续 count 个
                for (uint64_t i = start; i < start + count; i++) bm_set(bitmap_, i);
                free_pages_ -= count;
                return start * PAGE_SIZE;
            }
        } else {
            run = 0;                       // 遇到占用, 连续中断, 重新数
        }
    }
    return 0;                              // 找不到这么长的连续段
}
```

这是 O(总页数) 的扫描,比 `alloc_page` 慢得多,但 boot 期分配连续页的需求量不大(分配页表、大缓冲),可接受。如果哪天成了热点,再换 buddy 分配器这种专门优化连续分配的结构。`count == 1` 特意走 `alloc_page` 的快路径,免得单页分配也付线性扫描的代价。

释放 `free_page` 的重点不在速度,在**防御**:

```cpp
void PMM::free_page(uint64_t phys) {
    if (phys == 0) return;                 // 0 是失败哨兵, 不是真页, 不释放
    uint64_t idx = phys / PAGE_SIZE;
    if (idx >= highest_page_) return;      // 越界, 不动
    if (!bm_test(bitmap_, idx)) return;    // 本来就空闲 → double-free, 不动
    bm_clear(bitmap_, idx);
    free_pages_++;
}
```

三个 `return` 都是在挡非法释放:释放 0(那不是真页)、释放越界地址、double-free(这页没分配过又释放)。为什么要这么谨慎?因为 `free_pages_` 计数是靠「每次合法释放 +1」维护的,如果不挡 double-free,同一个页释放两次会让 `free_pages_` 虚增,账就乱了;更糟的是,double-free 后这页会被当成可分配,两个主人拿到同一页,数据互相覆盖。释放接口对非法输入「静默 no-op」而不是崩,是分配器的常规做法——内核崩在 `free` 里比啥都难查。host 单测专门有一组用例焊这些 no-op 行为(`free_page(0) is a no-op`、`double free is a no-op`)。

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
