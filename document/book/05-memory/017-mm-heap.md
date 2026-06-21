---
title: 017 · 在页上切块:内核堆分配器
---

# 017 · 在页上切块:内核堆分配器

> 上一章(016)我们让内核能主动控制虚拟↔物理映射了,但 PMM 和 VMM 的最小单位都是**一整页 4KB**。想存个 64 字节的结构体?拿一整页,浪费 4032 字节;想回收这 64 字节?你连「这页里哪段归谁」都没记录,根本无处下手。这一章,我们给内核装一个**堆分配器**:它在一串 VMM 映射好的页上做细粒度的切块、回收、合并,用 first-fit + 分裂 + 合并管理一块空闲链,空闲链用光了还会自动向 VMM 要更多页。再把全局 `operator new` / `delete` 接管到这套堆上——从此内核里的 C++ 代码写 `new` / `delete`,落到的就是自己的堆。内存子系统的第三块拼图就位。

## 这一章我们要点亮什么

三件事,一件比一件实在。

第一件,堆给了内核**任意大小**的动态分配能力。`g_heap.alloc(64)` 切一块 64 字节出来,`g_heap.free(p)` 还回去。不再被迫以页为单位,64 字节就只占 64 字节(外加一个小小的块头)。

第二件,这套分配器不只是「能分」,它还管「不浪费、不泄漏」。分配时如果切出来的块比需要的大很多,它会把尾部多余的**分裂**成一个新的空闲块留给下次;释放时如果发现旁边刚好也是空闲块,它会把两者**合并**成一块大的,避免碎片把内存切成渣。

第三件,稍微超出「分配器」本身的范围:我们把全局的 `operator new` / `delete` 全部重定向到这个堆上。于是在内核里写 `new Foo`、`new uint8_t[n]`、甚至带对齐要求的 `new (std::align_val_t(64)) T`,落到的都是 `g_heap.alloc`。C++ 的标准分配语法,在 freestanding 内核里第一次有了真实后端。

合起来,内核从「只能整页整页地拿内存」升级成「像用户态 `malloc` 一样按需切配」。后面几乎所有动态数据结构——动态数组、链表节点、各种缓存——都要建在这层之上。

## 为什么现在需要它

先接住 016 的尾巴。016 末尾我们留了一句话:现在最小单位是整页,「分配几十字节」还做不到——「那是下一章堆分配器的活」。这一章就是兑现这句承诺。PMM(015)解决了「物理页从哪来」,VMM(016)解决了「物理页怎么出现在虚拟地址空间」,两者配套,但粒度都停在页。堆就是在这两者之上加一层细粒度:它从 VMM 那儿**借**一串连续的页,自己在这串页里做字节级的切块回收。所以堆既不碰物理地址(那是 PMM 的事),也不碰页表映射(那是 VMM 的事),它只管「这一段已经映射好的虚拟内存里,哪些字节在用、哪些空着」。

再说说为什么是「借页」而不是「自己拥有固定一块内存」。内核启动时不知道以后要动态分配多少——文件系统的缓存、进程的内核栈、驱动里的各种缓冲,总量无法预知。所以堆被设计成**可扩张**:初始映射一小段(本章是 64 KB),不够了就再向 VMM 要几页续在末尾。这层「不够就扩」的设计,让堆不需要一上来就霸占一大块,也不需要在预测失误时推倒重来。它和 016 的 demand paging 是两种不同的「按需」:demand paging 是「访问到才补页」,堆的 expand 是「空闲链空了才扩页」,一个是被动缺页驱动,一个是主动容量驱动。

还有一笔账要交代清楚。`main.cpp` 里这一章的 milestone 注释写的是「Kernel heap allocator with **kmalloc/kfree**, new/delete takeover」。别被它带偏——翻遍这个 tag 的源码,**没有** `kmalloc` / `kfree` 这两个符号,那只是 milestone 目标里的叫法(沿袭了「内核 malloc」这个传统说法)。这一章实际落地的是 `Heap::alloc` / `Heap::free` 两个方法,加全局 `operator new` / `delete` 的接管。源码注释是线索,不是权威——这点我们在 010 章(GDT 注释把 SDM 图号抄错)就吃过亏,这里同理:以源码符号为准。

## 设计图

堆的核心数据结构是**空闲链表(free list)**:一块块连续的空闲区,用链表串起来。每个块前面顶着一个 `BlockHeader`,记录这块多大、是否空闲、下一块在哪。

```text
   一段 VMM 映射好的虚拟内存(初始 64 KB)
   base_                                                        base_ + size_
   ▼                                                                        ▼
   ┌────────────────────────────────────────────────────────────────────────┐
   │ BlockHeader │          可分配区(payload)                              │
   │ magic/size/ │   (初始时整段是一个大 free 块)                            │
   │ free/next   │                                                          │
   └────────────────────────────────────────────────────────────────────────┘
        │
        ▼
   free_list_ ──► (初始:就上面这一块, next=null)

   alloc(64) 之后:first-fit 找到这块, 它够大 → 从尾部切一块,
   原块缩成 remainder 留在链上, 切下来的块标 in-use 返回给调用者:

   ┌──────────────┬──────────┐┌──────────────┬──────────┐
   │ Header(rem)  │  free    ││ Header(in-use)│ 64 字节  │  ← 返回这块的 payload 指针
   │ size=大-开销 │ free=1   ││ size=64       │ free=0   │
   └──────────────┴──────────┘└──────────────┴──────────┘
        ▲
   free_list_ ──► remainder(remainder.next = null)

   free(p) 之后:把 in-use 块标回 free、塞回链头;若与相邻 free 块地址相连则 coalesce 合并
```

`BlockHeader` 本身长这样(32 字节):

```text
   ┌──────┬──────┬──────┬────────────┬──────────┐
   │magic │ size │ free │  _pad[12]  │   next   │   = 32 字节, [[gnu::packed]]
   │ 4B   │ 4B   │ 4B   │   12B      │   8B(x64)│
   └──────┴──────┴──────┴────────────┴──────────┘
     │      │      │                     │
     │      │      └─ 1=空闲 0=在用       └─ 空闲链的下一个块
     │      └─ payload 字节数(不含本头)
     └─ 0xDEADBEEF: 校验用, free() 时核对, 防 free 了野指针/双释放
```

alloc 的流程,关键是**对齐**——它让块头不再老实待在块的开头:

```text
   Heap::alloc(size, align=16):
     needed = size + (align-1)              ← 预留对齐填充
     沿 free 链 first-fit 找一块 free 且 size >= needed 的
     找到 curr(地址 curr_addr):
        block_end       = curr_addr + 32 + curr.size
        aligned_payload = align_up(curr_addr + 32, align)   ← 把 payload 摆到对齐处
        hdr_addr        = aligned_payload - 32              ← ★ 头紧贴 payload 之前
        usable          = block_end - aligned_payload       (须 >= size)
        front_pad       = hdr_addr - curr_addr              ← 块首到新头之间的缝
        tail_space      = block_end - (aligned_payload + size)
        从链上摘掉 curr
        若 front_pad >= 48(MIN_SPLIT): 把缝回收成小块塞回链; 否则: 这点缝丢了(内部碎片)
        若 tail_space >= 48: 把尾部 remainder 切成新 free 块塞回链
        在 hdr_addr 写 in-use 头, payload 清零, 返回 aligned_payload
     没找到 → expand() 向 VMM 续页, 递归重试 alloc
```

那个 `hdr_addr = aligned_payload - 32` 是这一章最容易写错、也最该讲透的地方——调试现场专门拎出来。

## 代码路线

### BlockHeader:32 字节的块头与 magic

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

### init:把一串页变成一个堆

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

### alloc:first-fit + 对齐 + 分裂

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

### 对齐的代价:front padding(本章核心坑)

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

### free:magic 校验、双释放检测、合并

`free` 短得多,但每一句都在防错:

```cpp
void Heap::free(void* ptr) {
    if (ptr == nullptr) return;                       // free(nullptr) 安全, 照搬 C 语义
    auto* block = header_from_ptr(ptr);               // ptr - 32
    if (block->magic != HEAP_MAGIC) {                 // 头被踩 / free 了野指针
        kprintf("[HEAP] Double-free or corruption at %p (magic=0x%x, expected 0x%x)\n", ...);
        return;
    }
    if (block->free) {                                // 已经是空闲块 → 双释放
        kprintf("[HEAP] Double-free detected at %p\n", ptr);
        return;
    }
    used_ -= HEADER_SIZE + block->size;
    block->free = 1;
    block->next = free_list_;                         // 塞回链头
    free_list_  = block;
    coalesce(block);                                  // 和相邻空闲块合并
}
```

两道校验。第一道 magic:如果 `ptr - 32` 处不是 `0xDEADBEEF`,说明这个指针压根不是堆分配出来的(野指针),或者这块的头已经被越界写坏了。第二道 `free` 标志:如果这个块**已经**标记为空闲,你又 free 一次,就是双释放——双释放会把同一块塞进链两次,后续分配可能把同一块返回给两个调用者,是堆腐败的头号元凶,这里直接拦下。

拦下之后是记账(`used_` 减)、回链(前插)、合并。前插(O(1))而不是排序插入,因为合并逻辑不依赖链的顺序——它按**地址**找邻居,不按链序。

`coalesce` 的逻辑值得看一眼,因为它暴露了这套实现的代价:

```cpp
void Heap::coalesce(BlockHeader* block) {
    bool changed = true;
    while (changed) {                                 // 合并可能级联, 多趟直到稳定
        changed = false;
        for (BlockHeader* curr = free_list_; curr; curr = curr->next) {
            // curr 的尾正好贴着 block 的头 → 把 block 并进 curr
            // 或 block 的尾正好贴着 curr 的头 → 把 curr 并进 block
            ...遇到相邻就改 size、从链上摘掉被并掉的那块、changed=true、break...
        }
    }
}
```

关键点:它判断「相邻」靠的是**地址**——`curr 的地址 + 头 + curr.size == block 的地址`。但 free 链**不是按地址排序的**(alloc 把 remainder 往链头塞,free 也往链头塞),所以要找地址邻居,只能**遍历整条链**;而且合并后可能产生新的相邻,所以外面套一层 `while (changed)` 多趟扫,直到没有新合并。复杂度是 O(块数²)。

对内核堆来说这通常无所谓——内核动态分配的块数量远达不到让 O(n²) 成为瓶颈的程度。但这是个明确的取舍:用「简单的前插链 + 全扫描合并」换「不用维护地址排序 / 边界标签」。工业级分配器(dlmalloc、Linux 的 slab/buddy)各有更精巧的办法(比如在块头里同时记物理前后邻居的「边界标签 footer」),这套实现没走那条路——够用、可读、好讲,是这一章的选择。这里只是点明它的上限,不是说它错了。

### expand:free 链空了就向 VMM 要页

`expand` 是「容量不够时自动长个」:

```cpp
void Heap::expand(size_t min_bytes) {
    uint64_t needed_bytes = align_up(min_bytes + HEADER_SIZE, PAGE_SIZE);
    uint64_t needed_pages = needed_bytes / PAGE_SIZE;
    if (needed_pages < EXPAND_PAGES) needed_pages = EXPAND_PAGES;   // 至少续 4 页(16 KB)
    uint64_t expand_size = needed_pages * PAGE_SIZE;

    for (uint64_t off = 0; off < expand_size; off += PAGE_SIZE) {   // 在 base_+size_ 处续映页
        uint64_t phys = g_pmm.alloc_page();
        if (phys == 0) { kprintf("[HEAP] OOM during expansion ...\n"); return; }
        g_vmm.map(base_ + size_ + off, phys, PAGE_FLAGS);
    }
    memzero((void*)(base_ + size_), expand_size);                   // 清零
    auto* nb = (BlockHeader*)(base_ + size_);                       // 新区摆一个 free 块
    nb->magic = HEAP_MAGIC; nb->size = expand_size - HEADER_SIZE; nb->free = 1; nb->next = free_list_;
    free_list_ = nb;
    size_ += expand_size;                                           // 总长增长
}
```

它和 `init` 高度同构:都是「要页 → 映射 → 清零 → 摆 free 块」,区别只在 `init` 是从头建、`expand` 是在 `base_ + size_`(当前末尾)续上。续完之后 `size_` 增长,新 free 块塞进链头,控制权回到 `alloc` 的递归调用——下一次 first-fit 就能扫到这块新区。

要诚实说明的是:**这套扩容只在内核里能真正跑起来**(它依赖 `g_pmm` / `g_vmm`),而且我们这一章自带的测试,分配量都落在初始 64 KB 之内,未必会触发 `expand`。它是一段「写好、接好、等被用到」的代码——逻辑正确性靠代码审查 + 下游真正压满时验证,而不是靠本 tag 的测试用例直接覆盖。这不算缺陷,但读的时候心里要有数:别以为「测试过了」就等于「扩容路径被实测过」。

### crt_stub:让 new/delete 落到堆上

最后一块拼图在 [crt_stub.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/crt_stub.cpp)。内核是 `-ffreestanding -nostdlib` 编译的,标准库的 `operator new` / `delete` 不存在,得自己提供。这一章把所有重载都接到堆上:

```cpp
void* operator new(unsigned long size)                          { return cinux::mm::g_heap.alloc(size); }
void* operator new[](unsigned long size)                        { return cinux::mm::g_heap.alloc(size); }
void* operator new(unsigned long size, std::align_val_t a)      { return cinux::mm::g_heap.alloc(size, (size_t)a); }
void* operator new[](unsigned long size, std::align_val_t a)    { return cinux::mm::g_heap.alloc(size, (size_t)a); }
void  operator delete(void* p) noexcept                         { cinux::mm::g_heap.free(p); }
void  operator delete[](void* p) noexcept                       { cinux::mm::g_heap.free(p); }
// ... 还有 sized / aligned 的 delete 变体, 都落到 free ...
```

两件事。一是覆盖了**对齐版**的 `operator new(size, std::align_val_t)`——这是 C++17 加进来的带对齐分配,当你 `new (std::align_val_t(64)) T` 或对齐要求超过 `__STDCPP_DEFAULT_NEW_ALIGNMENT__`(通常 16)的类型 `new` 时,编译器会调这个重载。它把对齐值透传给 `g_heap.alloc` 的第二个参数——这正是前面那一大段「front padding」机制存在的意义:堆得能按调用者要的任意对齐摆 payload。二是 `operator delete` 全部忽略尺寸 / 对齐参数(`free` 用不上,尺寸从头里读),不管编译器调哪个 delete 重载,都安全落到 `g_heap.free`。

这个文件里还住着别的 freestanding 桩(`__cxa_pure_virtual`、`__stack_chk_fail`、`__cxa_atexit`、`_init_global_ctors`),它们和堆没有直接关系,但和「让 C++ 在裸机上跑起来」是一体的——这一章顺手把 `new`/`delete` 也补齐了,内核的 C++ 运行时支持至此基本成形。

## 调试现场

这一章和 016 一样没有 notes 文件,但堆分配器有几个「写错就极难查」的隐患,值得当调试现场讲。

**一是 front padding 没摆正——头不在 payload 前 32 字节处。** 这是这一章最深的坑,测试里 [test_heap.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_heap.cpp) 专门标了 `CRITICAL: alloc with front padding` 的用例来盯它(kernel 侧那条 `test_odd_sizes` 的注释更直白,管它叫「the alignment padding bug」)。症状是这样的:如果你实现 alloc 时图省事,把块头固定摆在块首 `curr_addr`,payload 摆到中间某个对齐位置,却没有保证「payload - 32 正好是头」——那么 `free(p)` 调 `header_from_ptr` 算出来的头地址,落到了 payload 和块首之间的填充区,读出来的 `magic` 是随机字节。于是你会看到一个稳定的 `[HEAP] Double-free or corruption at ... (magic=0x...)`——明明只 free 了一次,却报「损坏」。更阴的是,如果那段填充区恰好某些字节拼出来等于 `0xDEADBEEF`(概率低但非零),magic 校验放过去了,`free` 就会改写一个错误位置的块头,静默踩坏邻居,等症状爆发时离根因十万八千里。根因就一句:**payload 的头必须写在 `aligned_payload - HEADER_SIZE`,`header_from_ptr` 才能找回来**。测试里先 `alloc(64)` 占点位置、再 `alloc(64, 4096)`(强对齐,逼出 front padding),然后 free 它、再验 `used_` 归零——这条链路要是断了,十有八九就是头摆错了。

**二是 magic 能抓什么、抓不了什么,要心里有数。** magic 是个「校验哨兵」,不是 fence(篱笆)。它能抓:free 一个非堆指针、free 一个头被改写的块、双释放(靠 `free` 标志,第二道)。它**抓不了**:你往自己分到的 buffer 末尾多写了几个字节,踩坏了**下一块**的头——这种越界,只有等那块下一块被 `free`(或被 alloc 扫到)时,magic 校验才会引爆,中间有一段延迟,而且报错位置(`[HEAP] ... corruption at <下一块的指针>`)离真正越界的地方(`上一块的末尾`)隔了一个块,容易误导排查。工业级分配器会在每块末尾再放一个「canary」/fence 字节,写越界会立刻破坏 canary、free 时立刻发现;这套实现没做,所以越界抓得晚。知道这个边界,调试时就不会一头扎进「为什么报错的块明明没动过」。

**三是 `used_` 和 `dump_stats` 不计 front-pad 碎片,账目对不齐别慌。** `used_` 只累加 `HEADER_SIZE + size`,不含被丢弃的 front padding;`dump_stats` 的 `free` 统计只遍历 free 链上的块,那些 `< 48` 字节的死缝也不在链上。所以 `used_ + free_total + 头开销` 未必等于总长,差出来的就是内部碎片。host 测试里有一条 `size accounting invariant`,断言用的是 `free_total + block_count * HEADER_SIZE <= total` 且 `free_total > total * 90%`——留了 10% 的碎片 slack,正是为了容纳这种 front-pad 损耗。你要是看见账目差了几十字节,先别怀疑实现错了,想想是不是奇数大小的连续分配攒下的死缝。

**四是 coalesce 的 O(n²) 在块特别多时会慢,但不会错。** 前面讲过,合并靠全链扫描 + 多趟。它的正确性没问题(地址相邻判断是精确的),但如果你某天发现内核某个分配密集的路径变慢,而 profile 指向 `coalesce`,不用怀疑逻辑——就是这个全扫描的设计到上限了。这时候该想的是「换数据结构(边界标签/按地址排序)」,而不是「调试合并逻辑」。这是已知取舍,不是 bug。

## 验证

堆的核心逻辑(first-fit、对齐、分裂、合并、双释放、账目)绝大部分能在 host 上镜像测。[test_heap.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_heap.cpp) 把堆算法用一块 host `calloc` 出来的缓冲(代替 VMM 映射的页)抄了一份,mock 掉 PMM/VMM,`-O2` 编、`CINUX_HOST_TEST` 门控,覆盖:默认 / 自定义对齐(16、64、4096)、分裂后 remainder 合法、free 降低 used、相邻块合并(正序/逆序/三块各种顺序)、双释放不腐败 used、magic 校验、200 轮交错 + 100 块填满再排空的应力、4096 对齐下的 front padding(header_from_ptr 仍能找回头)、耗尽后返回 nullptr、多块不重叠、账目不变量。

跑它们:

```bash
ctest --test-dir build -R heap --output-on-failure
```

这里要诚实标注一条边界:host 镜像用**固定**缓冲,**不测 `expand`**(它没法 mock 出 VMM 续页),耗尽就直接返回 nullptr。所以「自动扩容」这条路径,host 单测是验不到的——它的正确性靠代码审查和下游实测。

「真 VMM、真 PMM、真 new/delete」则在 QEMU 里验。[test_heap.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_heap.cpp) 在机内跑真正的 `g_heap`,9 个场景:基础分配 + 读写回测、默认 / 4096 对齐、`alloc(0)`→nullptr、`free(nullptr)` 安全、多块互不重叠、free 三小块后再分配大块(coalesce + 复用)、奇数大小(37/23/41)仍对齐、50 块应力(隔一块 free 一块、再回填、验存活块的标记)、`dump_stats` 不崩:

```bash
cmake --build build --target run-big-kernel-test
```

`g_heap.init` 会打一行 `[HEAP] Initialised at 0x..., size 64 KB`,看到它就说明堆建起来了;test section `Heap Tests (017)` 全过、末尾 `ALL TESTS PASSED`,说明 alloc/free/对齐/合并/双释放这一套在真硬件语义下成立。配合 host 单测焊死算法、机内测跑真后端,两层缺一不可——尤其对齐那一段,host 和机内都盯得很紧。

## 下一站

内存子系统的三块拼图现在齐了:PMM 管物理页的分配,VMM 管虚拟↔物理的映射(还顺手给了 demand paging),堆在两者之上做细粒度的切块回收,还接管了 `new`/`delete`。内核从「只能整页拿内存」变成了「像用户态一样按需 new」。

但你会注意到一个还没收口的细节:这一章堆的基址是硬编码的 `0xFFFF800000000000`——我们只是「把堆映射到 high-half 起点」,内核并没有一个「地址空间」的概念:哪段虚拟地址归内核、哪段留给将来用户态进程、堆区和别的区域会不会撞、每个进程要不要有自己的视图……这些问题,堆本身回答不了,它只是个「在一块已映射内存上切蛋糕」的工具。

下一站就是把这些收口:把「地址空间」正式抽象出来,让内核有一套统一的区域划分与映射管理。那是 018 的事——我们先享受一下「内核能 `new` 了」这个里程碑,地址空间下一章再见。

---

### 参考

- cppreference — [C++ `operator new` / `std::align_val_t`](https://en.cppreference.com/w/cpp/memory/new/operator_new):C++17 引入的带对齐 `operator new(size, std::align_val_t)` 重载语义,支持 `crt_stub.cpp` 里那组对齐版重定向、以及 `alloc` 第二参数 `align` 的来历。
- 015 章 · [给物理内存建账本:bitmap PMM](015-mm-pmm.md):堆的 `init` / `expand` 每页都靠 `g_pmm.alloc_page` 提供物理页,三块基石的第一块。
- 016 章 · [把物理页挂进虚拟地址:VMM](016-mm-vmm.md):堆的页靠 `g_vmm.map` 挂到虚拟地址;`0xFFFF800000000000` 这个 high-half 基址的由来、`phys_to_virt` 的自举约定,都在这一章打过底。
- 本 tag 源码:[heap.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/heap.hpp) / [heap.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/heap.cpp)、[crt_stub.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/crt_stub.cpp)(`operator new`/`delete` 重定向)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp)(Step 9 `g_heap.init`,生产基址 `0xFFFF800000000000`);测试 [test_heap.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_heap.cpp)(host 镜像,不含 expand)、[test_heap.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_heap.cpp)(QEMU 真 `g_heap`)、[main_test.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/main_test.cpp)(测试 harness 基址 `0xFFFFFFFF80100000`,与生产不同)。
