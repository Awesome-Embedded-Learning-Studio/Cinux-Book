---
title: 02 · 代码路线:BlockHeader / init / alloc / 对齐
---

# 代码路线:BlockHeader / init / alloc / 对齐

## BlockHeader:32 字节的块头与 magic

先看 [heap.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/heap.hpp) 里的块头定义:

```cpp
struct [[gnu::packed]] BlockHeader {
    uint32_t magic;
    uint32_t size;       // payload 字节数, 不含本头
    uint32_t free;       // 1 = 空闲, 0 = 在用
    uint8_t  _pad[12];   // 填充到 32 字节
    BlockHeader* next;   // 空闲链里的下一块
};
static_assert(sizeof(BlockHeader) == 32, "BlockHeader must be 32 bytes");
```

几个设计选择值得说。`size` 记的是 **payload** 大小,**不含**头本身——这样 `used_` 统计、coalesce 时算相邻,口径统一(块的「物理」总长 = `HEADER_SIZE + size`)。`free` 用一个独立的标志位,而不是靠「在不在 free 链上」来判断——因为 free 的时候要先检查它,才能发现双释放(一个已经在链上的块又被 free 一次)。`magic = 0xDEADBEEF` 是校验哨兵,`free()` 时核对,用来抓「free 了一个野指针」或「块头被踩坏」。

为什么凑成 32 字节?两个理由。一是 `next` 指针在 64 位上是 8 字节,放最后;前面三个 4 字节字段 + 12 字节填充正好让总长落到 32,是个对齐友好的 2 的幂。二是 32 本身是 16 的倍数——这就让默认的 16 字节对齐「自然成立」:只要块头本身摆在 16 字节对齐的地址上,payload(头之后)也就是 16 字节对齐的。`_pad[12]` 看着多余,它就是为了让总长精确到 32、把 `next` 顶到末尾;`[[gnu::packed]]` 防编译器自作主张加对齐填充把它撑大。`static_assert` 把「必须 32 字节」焊死,谁改了字段得立刻发现。

## init:把一串页变成一个堆

[heap.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/heap.cpp) 的 `init` 做的是「把一段虚拟地址空间变成堆」:

```cpp
void Heap::init(uint64_t virt_base, uint64_t initial_size) {
    uint64_t aligned_size = align_up(initial_size, cinux::arch::PAGE_SIZE);   // 1. 页对齐
    for (uint64_t off = 0; off < aligned_size; off += PAGE_SIZE) {            // 2. 每页: 要物理页 + 映射
        uint64_t phys = g_pmm.alloc_page();
        if (phys == 0) { kprintf("[HEAP] OOM during init ...\n"); return; }
        g_vmm.map(virt_base + off, phys, PAGE_FLAGS);                         //    PAGE_FLAGS = present+writable
    }
    memzero((void*)virt_base, aligned_size);                                  // 3. 整块清零
    auto* first = (BlockHeader*)virt_base;                                    // 4. 摆一个覆盖全区的 free 块
    first->magic = HEAP_MAGIC;
    first->size  = aligned_size - HEADER_SIZE;
    first->free  = 1;
    first->next  = nullptr;
    base_ = virt_base; size_ = aligned_size; used_ = 0; free_list_ = first;   // 5. 记账
}
```

五步,每步都有它存在的理由。第一步页对齐,因为后面是按页映射的,`initial_size` 不是页的整数倍没法直接切。第二步把每个虚拟页挂上一个真实物理页——这里就显出了堆对 PMM + VMM 的依赖:它不自己造内存,只向 `g_pmm` 借物理页、让 `g_vmm` 挂到虚拟地址上。第三步**整块清零**——和 016 章「新建页表必须清零」同一个道理:不清零,残留字节会被当成上一个主人留下的数据,`BlockHeader` 里的 `magic` 就可能是任意值。第四步摆一个覆盖整段的初始 free 块,这块的 `size` 是 `整段 - 头`。第五步记账。

注意 `PAGE_FLAGS = 0x03`,即 present(bit0)+ writable(bit1)——堆区既要能访问又要能写,但**没有** user 位,它是内核私有的。

调用点在 [main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp) 的 Step 9:

```cpp
constexpr uint64_t HEAP_VIRT_BASE     = 0xFFFF800000000000ULL;
constexpr uint64_t HEAP_INITIAL_SIZE  = 64 * 1024;          // 64 KB
cinux::mm::g_heap.init(HEAP_VIRT_BASE, HEAP_INITIAL_SIZE);
```

`0xFFFF800000000000` 是 x86-64 虚拟地址空间里一个有讲究的数:它是「规范高半区(canonical high half)」的起点——bit 47 为 1、往高位全 1 的区域,内核传统上把内核自己的数据摆在高半区。这一章我们只是「把堆映射到这个地址」,并没有一套「地址空间」的抽象;把这个基址正式化、和别的区域一起管理,是下一章(018)的事,这里不展开。先记住:堆在高半区起点,初始 64 KB。

(题外话但不该漏:测试 harness [main_test.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/main_test.cpp) 里 `g_heap.init` 用的基址是 `0xFFFFFFFF80100000`——靠近内核镜像、和 016 的 `KERNEL_VMA` 一脉相承的那片,和生产 `main.cpp` 的 `0xFFFF800000000000` **不是同一个**。生产路径和测试路径各选了各自方便的虚拟地址,这是正常的,别在读源码时把它们当成同一个数。)

## alloc:first-fit + 对齐 + 分裂

`alloc` 是这一章最长、也最该读慢的方法。策略是 **first-fit**:沿空闲链找**第一个**放得下的空闲块,不挑最优(那是 best-fit,省碎片但慢)。找到后处理对齐和分裂:

```cpp
void* Heap::alloc(size_t size, size_t align) {
    if (size == 0) return nullptr;            // 0 字节直接拒
    if (align < 16) align = 16;               // 最低 16 字节对齐
    size_t needed = size + (align - 1);       // 预留对齐填充的余量

    BlockHeader* prev = nullptr;
    BlockHeader* curr = free_list_;
    while (curr != nullptr) {
        if (curr->magic != HEAP_MAGIC) { ... return nullptr; }   // 链被踩坏
        if (curr->free && curr->size >= needed) {
            uintptr_t curr_addr      = (uintptr_t)curr;
            uintptr_t block_end      = curr_addr + HEADER_SIZE + curr->size;
            uintptr_t aligned_payload= align_up(curr_addr + HEADER_SIZE, align);
            uintptr_t hdr_addr       = aligned_payload - HEADER_SIZE;
            size_t usable = block_end - aligned_payload;
            if (usable < size) { prev = curr; curr = curr->next; continue; }   // 对齐后放不下了, 看下一块

            size_t front_pad  = hdr_addr - curr_addr;
            size_t tail_space = block_end - (aligned_payload + size);

            // 摘掉 curr
            if (prev) prev->next = curr->next; else free_list_ = curr->next;
            // 头部那条缝够大就回收成小块, 否则丢弃(内部碎片)
            if (front_pad >= MIN_SPLIT) { curr->size = front_pad - HEADER_SIZE; curr->next = free_list_; free_list_ = curr; }
            // 尾部 remainder 够大就切成新 free 块
            if (tail_space >= MIN_SPLIT) { auto* rem = (BlockHeader*)(aligned_payload + size); ...; free_list_ = rem; }

            // 在 hdr_addr 写 in-use 头
            auto* h = (BlockHeader*)hdr_addr;
            h->magic = HEAP_MAGIC; h->size = size; h->free = 0; h->next = nullptr;
            used_ += HEADER_SIZE + size;
            memzero((void*)aligned_payload, size);      // 分出来的内存清零
            return (void*)aligned_payload;
        }
        prev = curr; curr = curr->next;
    }
    expand(size + align + HEADER_SIZE);     // 链空了 → 扩容
    return alloc(size, align);              // 递归重试
}
```

`MIN_SPLIT = HEADER_SIZE + 16 = 48`:一条缝只有大到「塞得下一个头(32)+ 至少 16 字节 payload」才值得切成独立小块,否则切出来是个永远没人要的碎渣,反而增加链长和碎片——不如直接丢掉那点字节。这是「最小可分裂块」的常见阈值。

`needed = size + (align - 1)` 是对齐的余量预算:把 payload 上对齐最多会浪费 `align - 1` 字节,所以先按最坏情况算「这块够不够」。后面 `usable` 才是精确算「对齐之后真正剩下的可用空间」。

末尾的 `expand` + 递归 `alloc` 是「自动扩容」:free 链里没有放得下的块,就向 VMM 续页、再重试一次。`expand` 一次至少续 4 页(16 KB),而且按请求大小算够需要的页数,所以重试一次基本就能成功。注意这里**不是循环**而是递归——`expand` 之后直接 `return alloc(size, align)`,语义上就是「扩完再分一次」。

## 对齐的代价:front padding(本章核心坑)

整段 `alloc` 里最该停下来想的是这两行:

```cpp
uintptr_t aligned_payload = align_up(curr_addr + HEADER_SIZE, align);
uintptr_t hdr_addr        = aligned_payload - HEADER_SIZE;
```

为什么不是「头摆在块首 `curr_addr`、payload 紧跟其后」?因为如果那样,payload 的地址就是 `curr_addr + 32`,它**不一定满足调用者要的对齐**。比如调用者要 4096 字节对齐(`new (std::align_val_t(4096)) T`),而 `curr_addr + 32` 一般不是页对齐的——你只能把 payload 往后挪到最近一个对齐处,于是头就得跟着挪到 `payload - 32`。这一挪,块首(`curr_addr`)和新头(`hdr_addr`)之间就空出了一段 `front_pad`。

这个 `front_pad` 有三种命运:

- **`front_pad == 0`**(对齐天然成立,比如默认 16 对齐且块首本就 16 对齐):完美,头还在块首,没有任何浪费。
- **`front_pad >= 48`**(对齐要求大,缝比较宽):这段缝被回收成一个独立的小 free 块塞回链,下次还能用,几乎不浪费。
- **`0 < front_pad < 48`**:既放不下一个有效块,又没法忽略,这段字节就成了**内部碎片**,永远没人用——`dump_stats` 看不到它,`used_` 也不计它,纯丢失。在默认 16 字节对齐下,这块缝最多 15 字节(奇数大小的连续分配,比如 `alloc(37)` 之后下一块的块首就不再 16 对齐,就会踩出这种个位数到十几个字节的缝);换更大的对齐(32、64)时缝才可能到几十字节——但那种情况下缝通常已经 ≥ 48,会被前一条规则回收掉。

而这一切之所以能自洽,全靠一个约定:**块的 payload 指针往前数 32 字节,就一定是它自己的头**。`free` 就是靠这个把指针还原成头的:

```cpp
BlockHeader* header_from_ptr(void* ptr) {
    return (BlockHeader*)((uintptr_t)ptr - HEADER_SIZE);   // ptr - 32
}
```

所以 `free(p)` 并不需要你告诉它「这块多大」——头就在 `p - 32` 处,大小写在头里。这也是为什么 `operator delete(void*)` 哪怕不带尺寸也能工作:尺寸从头里读。

这一段的坑在「调试现场」展开——如果你写 alloc 时,头摆在块首、payload 摆在中间某个对齐处,却没有让「payload 前 32 字节正好是头」,`header_from_ptr` 就会读到错位的字节,后果是 magic 校验莫名其妙失败、或者静默踩坏邻居。
