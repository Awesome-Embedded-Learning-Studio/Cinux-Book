---
title: 02 · 代码路线:从 E820 到 bitmap 分配
---

# 代码路线:从 E820 到 bitmap 分配

## 先搞清有哪些内存可用:解析 E820

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

## bitmap 放哪:自举的鸡生蛋问题

拿到 usable region 之后,要为整个物理地址空间建 bitmap。bitmap 多大?看最高物理地址:`highest_page_ = max_addr / PAGE_SIZE`,`bitmap_size_ = (highest_page_ + 7) / 8`——最高物理地址对应的页号,除以 8(每字节 8 位),就是 bitmap 字节数。比如最高物理地址 4GB,就是 4GB/4KB = 1M 页,bitmap = 128KB,不大。

接下来是个微妙的「鸡生蛋」问题:**bitmap 自己也是一块内存,它该放在哪?** 你要管理内存,得先有一块内存放管理结构;但「分配内存」这件事还没就绪。这就是自举(bootstrap)的经典困境。

Cinux 的解法很务实:把 bitmap 放在 **kernel 栈顶(`__kernel_stack_top`)之后、页对齐**的地方。为什么放这里?因为这块区域是 bootloader/链接脚本早就映射好了的、内核此刻就能访问的——PMM 还没就绪时,只有这种「已映射」的位置是安全的。如果随手把 bitmap 放到一个没映射的虚拟地址,`init` 里第一个写 bitmap 的动作就 page fault 了,而此刻 PMM 自己还没起来,连崩都没法好好崩。

```cpp
uintptr_t stack_top_virt = reinterpret_cast<uintptr_t>(&__kernel_stack_top);
uintptr_t bm_virt = (stack_top_virt + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);  // 页对齐
bitmap_ = reinterpret_cast<uint8_t*>(bm_virt);
```

这里出现了一个关键的常数 `KERNEL_VMA = 0xFFFFFFFF80000000`——内核跑在「高半区」(higher-half),虚拟地址和物理地址之间差这个偏移。bitmap 放在虚拟地址 `bm_virt`,它对应的物理地址就是 `bm_virt - KERNEL_VMA`。这个换算在 `init` 后面会用到(要把 bitmap 自身标占用,得知道它的物理地址)。`KERNEL_VMA` 是个写死的约定,它和 013 里 `map_mmio` 那两个写死的页表地址是同一类东西——都是「bootloader/链接脚本布局约定」的硬编码,脆弱但在 boot 期够用。

## init 的反向思路:先全占用,再 carve 出可用

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

Step 6 和 Step 7 是两个容易漏、漏了就翻车的点。kernel image(代码、数据)、栈,它们物理上确实落在某个 usable region 里(E820 觉得「这是 RAM,可用」),但它们此刻正被内核用着,绝不能分配出去。bitmap 自己也一样——它就放在栈顶后面,也是 usable RAM,但它是分配器自己的账本,发出去等于自毁。所以 init 显式地「把这些我们已经占的位置重新标占用」。漏掉 Step 7(bitmap 自身),`alloc_page` 早晚会把 bitmap 自身的物理页当成空闲页分配出去,接下来你清零一个「分配到的页」,实际清的是 bitmap,分配器当场错乱——调试现场会专门讲这个。

## 分配与释放:64 位 ctzll 加速 + 连续分配

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
