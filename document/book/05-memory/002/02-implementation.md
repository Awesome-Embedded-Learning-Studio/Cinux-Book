---
title: 02 · 代码路线:paging_config / PageEntry / walk / demand paging
---

# 代码路线:paging_config / PageEntry / walk / demand paging

## 先把页表常量理清:paging_config

x86-64 分页有一堆固定的常数:页大小、各级索引的移位、页表项里哪些位是物理地址、哪些位是 flag。这一章把这些抽进了 [paging_config.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/paging_config.hpp),让 VMM 和别处共用同一套定义,不再各写各的魔法数:

```cpp
constexpr uint64_t PAGE_SIZE  = 4096;
constexpr uint32_t PT_SHIFT=12, PD_SHIFT=21, PDPT_SHIFT=30, PML4_SHIFT=39;  // 每级索引的移位
constexpr uint64_t ADDR_MASK = 0x000FFFFFFFFFF000ULL;   // PTE 里物理地址所在的位 [12..51]

constexpr uint64_t FLAG_PRESENT  = 1ULL << 0;   // 存在位
constexpr uint64_t FLAG_WRITABLE = 1ULL << 1;   // 可写
constexpr uint64_t FLAG_USER     = 1ULL << 2;   // 用户态可访问
constexpr uint64_t FLAG_HUGE     = 1ULL << 7;   // 大页
constexpr uint64_t FLAG_NX       = 1ULL << 63;  // 不可执行

constexpr uint64_t PML4_INDEX(uint64_t v) { return (v >> PML4_SHIFT) & 0x1FF; }  // 9 位 = 512
// PDPT_INDEX / PD_INDEX / PT_INDEX 同理
```

几个要点。每级索引 9 位(`& 0x1FF`),正好 512 个条目——这是 4 级页表每级 512 项的由来。`ADDR_MASK = 0x000FFFFFFFFFF000`:页表项是 8 字节(64 位),低 12 位是 flag(P、RW、US 等),bit 12 到 bit 51 这 40 位是物理页的基地址(物理地址按 4KB 对齐,低 12 位永远是 0,所以不需要存),高位(bit 52-62 是 reserved/flag,NX 在 bit 63)。`ADDR_MASK` 正好框住 `[12..51]` 这段物理地址位。`PML4_INDEX(v)` 这类宏,把虚拟地址的某一段抠出来当某级表的下标——比如 PML4 索引就是虚拟地址右移 39 位再取低 9 位。把这些常量和宏集中起来,后面写 walk 就清爽了。

## 页表项 PageEntry:一个 8 字节的联合体

页表项本身,在这一章被包成了一个 `PageEntry` 联合体([paging.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/paging.hpp))。它本质就是一个 8 字节的整数(`raw`),但提供了几个方便的访问器:

```cpp
union PageEntry {
    uint64_t raw;
    // ... 按位的结构体(可选, 本章主要用 raw + 访问器) ...
    uint64_t phys_addr() const { return raw & ADDR_MASK; }      // 取出物理地址
    void set_phys_addr(uint64_t p) { raw = (raw & ~ADDR_MASK) | (p & ADDR_MASK); }
    bool is_present() const { return (raw & FLAG_PRESENT) != 0; } // 存在?
};
static_assert(sizeof(PageEntry) == 8, "...");   // 页表项必须 8 字节
```

为什么用联合体?因为页表项可以「整体当一个 64 位数」操作(`raw = phys | flags`),也可以「按位域」操作(单独改某个 flag)。这一章 VMM 主要用整体操作——映射时直接 `entry.raw = (phys & ADDR_MASK) | flags`,把物理地址和 flag 拼成一个 64 位数塞进去。`phys_addr()` 反过来用 `ADDR_MASK` 把物理地址抠出来,`is_present()` 看最低位。这几个访问器把「PTE 的位布局」这个细节藏起来,VMM 代码读着就是「这个表项指向哪、在不在」,不用每次手写位运算。`flush_tlb(virt)`(一条 `invlpg`)和 `read_cr3()`(读 CR3 寄存器)也在 paging.hpp 里,是分页操作的最底层原语。

## map/unmap/translate:走四级,缺表就建

有了常量和 PageEntry,VMM 的三个操作都是「走四级页表」的变种。看 [vmm.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/vmm.cpp) 的 `map`:

```cpp
bool VMM::map(uint64_t virt, uint64_t phys, uint64_t flags, uint64_t* pml4) {
    uint64_t pml4_phys = pml4 ? *pml4 : kernel_pml4_;        // 默认用内核 PML4
    auto* pml4_table = phys_to_virt(pml4_phys);

    auto* pdpt = walk_level(pml4_table, PML4_INDEX(virt), true);  // true = 缺表就建
    if (!pdpt) return false;
    auto* pd = walk_level(pdpt, PDPT_INDEX(virt), true);
    if (!pd) return false;
    auto* pt = walk_level(pd, PD_INDEX(virt), true);
    if (!pt) return false;

    pt[PT_INDEX(virt)].raw = (phys & ADDR_MASK) | (flags & ~ADDR_MASK);  // 末级:挂物理页
    flush_tlb(virt);
    return true;
}
```

四级走法一目了然:PML4 → PDPT → PD → PT,每一级用虚拟地址对应段的索引取下一级表的地址。前三级调 `walk_level(..., true)`——`true` 表示「这一级的表项如果不存在,就从 PMM 现建一个」。最后一级(PT)不走路,直接把物理页挂上:`(phys & ADDR_MASK)` 取物理地址位,`(flags & ~ADDR_MASK)` 取 flag 位(用 `~ADDR_MASK` 确保 flag 不会污染到物理地址位),拼起来写进 PT 项。最后 `flush_tlb(virt)`,作废这条地址的 TLB 缓存,让 CPU 立刻看到新映射。

`walk_level` 是这套机制的心脏,它处理「中间表不存在就建」:

```cpp
PageEntry* walk_level(PageEntry* table, uint64_t index, bool should_alloc) {
    PageEntry& entry = table[index];
    if (entry.is_present())                       // 这级表项已在 → 走下一级
        return phys_to_virt(entry.phys_addr());
    if (!should_alloc) return nullptr;            // 不让建 → 停(translate/unmap 用)

    uint64_t new_page = g_pmm.alloc_page();       // 中间表也是一页物理内存
    if (new_page == 0) return nullptr;            // PMM 没页了 → 失败

    auto* new_table = phys_to_virt(new_page);
    for (uint32_t i = 0; i < PT_ENTRIES; i++)      // ★ 必须清零
        new_table[i].raw = 0;

    entry.raw = new_page | FLAG_PRESENT | FLAG_WRITABLE;  // 链接进上一级
    return new_table;
}
```

三个细节决定了它能不能对。第一,中间表(一张 PML4/PDPT/PD/PT)本身就是**一页 4KB 物理内存**,512 个 8 字节条目正好 4KB,所以从 PMM 要一页就够了。第二,新建的表**必须清零**——不清零,内存里残留的随机位会被 `is_present()` 误判为「这条映射在」,walk 就跟着野指针飞出去了。这是这一章的头号隐患,调试现场专门讲。第三,把新表链接进上一级时,`entry.raw = new_page | PRESENT | WRITABLE`:中间表项要置 PRESENT(否则 walk 到这就停了)和 WRITABLE(后面可能要改下级表)。

这套 walk 还藏着一个自举技巧:`phys_to_virt`。

```cpp
PageEntry* phys_to_virt(uint64_t phys) {
    return reinterpret_cast<PageEntry*>(phys + KERNEL_VMA);   // KERNEL_VMA = 0xFFFFFFFF80000000
}
```

页表存在物理内存里,`walk_level` 拿到的「下一级表地址」是**物理**地址,但内核代码只能访问**虚拟**地址。怎么读到那张物理上的表?靠 `phys_to_virt`:把物理地址加上 `KERNEL_VMA`(高半区偏移),得到的虚拟地址正好映射回那块物理内存。这之所以能工作,是因为 bootloader 早就把(至少页表所在的)物理内存做了「物理地址 `p` ↔ 虚拟地址 `p + KERNEL_VMA`」的高半区映射。VMM 靠这条约定,才能用虚拟地址去读写那些「物理」的页表。这是个 boot 期约定,脆弱但必要——如果某张页表落在一个没做高半区映射的物理地址上,`phys_to_virt` 访问它就会缺页,而此刻正在处理缺页,递归下去就是 double fault。

`unmap` 和 `translate` 是同一套 walk,只是 `should_alloc = false`:走到哪级表不存在就停(返回 nullptr / 0)。`unmap` 把末级 PT 项清零再 `flush_tlb`;`translate` 返回 `phys_addr() | (virt & 0xFFF)`(物理页基址 + 页内偏移)。注意 `unmap` **只拆映射,不回收物理页**——它不知道这页是不是 caller 还在用,回收是 caller 的责任(注释里写明了)。这是个有意的分工:VMM 管映射,物理页的归还不归它。

## demand paging:page fault 里缺页即补

VMM 写好了,「主动 map」能用。但这一章最超出预期的,是把它接进了缺页异常,实现「被动」的 demand paging。

缺页异常是 vector 14(`#PF`),CPU 触发它时,会把引发缺页的虚拟地址放进 `CR2` 寄存器,把错误类型放进栈上的 error code。这一章的 `handle_pf`([exception_handlers.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/exception_handlers.cpp))在原来的「诊断 + 挂起」之前,加了一段:

```cpp
void handle_pf(InterruptFrame* frame) {
    uint64_t fault_addr;
    __asm__ volatile("movq %%cr2, %0" : "=r"(fault_addr));   // CR2 = 缺页地址
    uint64_t err = frame->error_code;

    // Demand paging: 对「页不存在」的缺页, 现场补一页
    if ((err & 0x01) == 0) {                                 // bit0=0 → not present
        uint64_t virt_page = fault_addr & ~0xFFFULL;          // 对齐到页
        uint64_t phys = g_pmm.alloc_page();
        if (phys != 0 && g_vmm.map(virt_page, phys, FLAG_PRESENT | FLAG_WRITABLE)) {
            kprintf("[VMM] Demand-paged %p -> phys %p\n", ...);
            return;                                           // 补好了, 返回, 那条指令重执
        }
    }
    // bit0=1 → protection violation(真错误), 走原来的诊断 + 挂起
    ...
}
```

关键是 error code 的 **bit0(P 位)**:它为 0 表示「引发缺页是因为这个页根本不存在」(可补);为 1 表示「页存在,但访问违规了」(比如写一个只读页、或权限不够)——后者是真正的 bug,不能靠补一页解决。所以 demand paging 只在 `bit0 == 0` 时尝试:从 PMM 要一页、用 VMM 映射到缺页地址、`return`。从中断返回后,CPU 重新执行那条引发缺页的指令,这次页已经在了,正常继续。

这个「缺页即补」是 demand paging 的最小形态。它的妙处在于:内核不用预先把所有可能访问的虚拟地址都映射好,「第一次访问时现场补」。配合 VMM 的「缺中间表自动建」,整条路径是全自动的——访问新地址 → 缺页 → 补 PT/PD/PDPT(走 walk_level 建中间表)+ 补末级页 → 返回成功。代价是第一次访问有一次缺页开销,之后正常。

但要清醒地看到它的边界:这是「缺页就分配一页物理内存」,**不是完整的换页(paging/swapping)**。它没有「内存不够时把不常用的页换到磁盘」、没有「换回来」、没有任何换出机制——物理内存分光了就是分光了,PMM 返回 0,demand paging 失败,只能走挂起。所以别把它和操作系统的 swap 混为一谈,它只是「懒分配」:把「预先映射」推迟到「第一次访问」。
