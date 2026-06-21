---
title: 016 · 把物理页挂进虚拟地址:虚拟内存管理器
---

# 016 · 把物理页挂进虚拟地址:虚拟内存管理器与 demand paging

> 上一章(015)我们给物理内存建了账本,能 `alloc_page` / `free_page` 了。但拿到的只是一个**物理**地址——你没法直接说「把这个物理页映射到某个虚拟地址」。直到现在,内核用的还是 bootloader 搭的那套固定页表;还记得 013 为了点亮 framebuffer,我们硬编码页表地址、塞了几个大页条目进去的 `map_mmio` 吗?那是个 hack。这一章,我们写一个正经的虚拟内存管理器(VMM):它能走 4 级页表,把任意物理页映射到任意虚拟地址、能取消映射、能翻译地址;更进一步,我们在缺页异常里接上它,实现「用到才分配」的 demand paging。从这一章起,内核才真正掌控了虚拟地址空间。

## 这一章我们要点亮什么

两件事,一件是能力,一件是惊喜。

第一件,VMM 给了内核**主动控制虚拟→物理映射**的能力。`map(virt, phys, flags)` 把一个物理页挂到某个虚拟地址;`unmap` 拆掉;`translate` 查一个虚拟地址实际落在哪个物理页。映射时如果中间的页表还不存在,它会自动从 PMM 要页、建表、链接——你不用预先把四级页表全建好。013 那个硬编码页表地址的 `map_mmio` hack,终于有了一套正经的替代能力。

第二件,稍微超出预期:我们在缺页异常(`#PF`)处理里接上了 VMM,实现了 **demand paging(按需调页)**。往后,凡是访问一个「还没映射」的虚拟页,CPU 触发缺页异常,我们的 handler 现场从 PMM 分配一页、用 VMM 映射上去、然后返回——那条指令重新执行,这次就成功了。于是内核不用预先为所有可能用到的地址建映射,「用到才给」。

这两件事合起来,意味着内核从「跑在一套写死的页表上」升级成了「自己管理虚拟地址空间」。这是后面进程、堆、独立地址空间的真正前提。

## 为什么现在需要它

先说为什么紧跟 015。015 让我们能分配物理页了,但物理页拿到手只是个物理地址,「把它挂到哪个虚拟地址」还没法控制。换句话说,PMM 解决了「物理页从哪来」,但没解决「物理页怎么出现在虚拟地址空间里」。这两件事必须配套:有 PMM 没 VMM,你拿到的物理页用不上(除非它正好已经在某个固定映射里);有 VMM 没 PMM,VMM 建中间页表时没处要页。所以 015 一就位,016 紧跟着就来,它俩是内存子系统的一对基石。

再回头看那个 `map_mmio`。013 为了点亮屏幕,我们写了个最小的页表助手:它硬编码了两个页表虚拟地址、往里塞大页条目、只做恒等映射、不管权限、不管回收。当时就说了「这是个 hack,等做了正经页表管理器就替掉它」。VMM 就是那个正经的管理器:它能走任意路径的 4 级页表、按需建表、精细控制 flag。`map_mmio` 那种「摸黑改固定地址」的做法,从此有了正经的替代者——不过这一章 framebuffer 那一路暂且还沿用旧的 `map_mmio`(它还能用,没必要为换而换),VMM 的 map 能力是为更往后的用途(给进程建独立地址空间、动态映射)铺的路,真正替换 framebuffer 那条路径要等后续重构。

还有一笔面向未来的账。VMM 的 `map` / `unmap` 都接受一个可选的 `pml4` 参数——也就是「在哪套页表里做映射」。这一章我们总用内核自己的那套,但这个参数是为以后留的:将来要给每个进程一套独立的页表(独立的虚拟地址空间),`map` 只要换个 `pml4` 就能往进程的页表里映射。所以这一章虽然只服务内核自己,但它把「多地址空间」的接口形状先定下来了。

## 设计图

VMM 的核心操作是「走 4 级页表」。x86-64 的虚拟地址被拆成五段:四级页表的索引各 9 位,加 12 位页内偏移。

```text
   虚拟地址 64 位
   ┌─────────┬─────────┬─────────┬─────────┬────────────┐
   │ PML4 idx│ PDPT idx│  PD idx │  PT idx │ page offset│
   │ [39..47]│ [30..38]│ [21..29]│ [12..20]│  [0..11]   │
   └────┬────┴────┬────┴────┬────┴────┬────┴─────┬──────┘
        ▼         ▼         ▼         ▼          ▼
   CR3→PML4 ──→ PDPT ──→  PD ──→  PT  ──→  物理页基址 + offset
   (每级 512 项; 不存在的中间表, map 时从 PMM 现建)

   walk_level(table, index, should_alloc):
        entry = table[index]
        if entry.is_present(): → phys_to_virt(entry.phys_addr())  走下一级
        else if should_alloc:
            new = PMM.alloc_page()         ← 中间表也是一页物理内存
            把 new 清零                       ← 必须清零, 否则陈旧位会被当成 present
            entry = new | PRESENT | WRITABLE (链接进上一级)
            → phys_to_virt(new)
        else: → nullptr  (translate/unmap 走到这里就停)

   phys_to_virt(phys) = phys + KERNEL_VMA   ← 靠高半区偏移访问「物理」页表(自举关键)
```

demand paging 则是另一条线,挂在缺页异常上:

```text
   访问一个未映射的虚拟地址
        ▼ CPU 触发 #PF(vector 14)
   handle_pf:
        fault_addr = CR2              ← CR2 存引发缺页的地址
        err = error_code
        if (err & 0x01) == 0:         ← bit0=0 表示「页不存在」(可补)
            phys = PMM.alloc_page()
            if phys && VMM.map(virt_page, phys, PRESENT|WRITABLE):
                return                 ← 补好了, 重新执行那条指令
        // bit0=1 是 protection violation(真错误), 走诊断 + 挂起
```

两条线的交汇点就是 VMM:`map` 是「主动映射」,demand paging 是「被动(缺页时)映射」,两者都调同一个 `VMM::map`。

## 代码路线

### 先把页表常量理清:paging_config

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

### 页表项 PageEntry:一个 8 字节的联合体

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

### map/unmap/translate:走四级,缺表就建

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

### demand paging:page fault 里缺页即补

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

## 调试现场

这一章没有 notes 文件,但 VMM 有三个写错就「极难查」的隐患,值得当成调试现场。

一是 **新页表没清零**,这是 VMM 最经典的坑。`walk_level` 从 PMM 拿到一页做新表,如果没那个清零循环,这页内存里是上一个主人留下的残留数据。那些残留的 64 位整数,被当成页表项后,其中凡是最低位(P 位)恰好是 1 的,都会被 `is_present()` 判成「这条映射在」,walk 就跟着里面的「物理地址」飞到一个随机位置。症状是:映射某个地址后,translate 出来一个莫名其妙的物理地址,或者写一个虚拟地址却改到了毫不相关的物理内存——而且每次重启表现还不一样(取决于那页内存上次的残留)。这种 bug 三分靠代码、七分靠运气,排查极痛苦。根因就一行:新建表后必须把 512 个条目全清零。养成「凡是新建页表,第一件事 memset/循环清零」的肌肉记忆,能省掉这一整类噩梦。

二是 **map/unmap 后忘了 flush_tlb**。CPU 把页表项缓存在 TLB 里,你改了内存里的 PTE,TLB 不会自动更新——CPU 还按旧映射走。于是出现诡异的「明明 unmap 了,访问还不缺页」「明明 map 了,访问还 page fault」,过一会儿又突然好了(TLB 被别的操作刷掉了)。这种「时序相关、忽好忽坏」的症状,九成是忘了刷 TLB。规矩:`map` 设完 PTE、`unmap` 清完 PTE,立刻 `flush_tlb(virt)`(一条 `invlpg`)作废这一页的 TLB 项。这一章的 `map`/`unmap` 末尾都跟了一句 flush,不是装饰。

三是 **物理地址没 mask,污染了 flag 位**。`pt[idx].raw = phys | flags`,如果你传进来的 `phys` 低 12 位不是 0(没按页对齐),或者没用 `ADDR_MASK` 过滤,那些位就会和 flag 位(P、RW、US…)撞车——比如 phys 的某位被当成了 PRESENT,或 flag 被当成了地址位。出来的映射要么权限错、要么地址错。VMM 的写法是 `(phys & ADDR_MASK) | (flags & ~ADDR_MASK)`:物理地址只留 `[12..51]`,flag 只留 `[0..11]` 和高位,两段不重叠,谁也污染不了谁。写 PTE 时永远用 mask 把两段分开拼,这是铁律。

还有一个和 demand paging 相关的:`phys_to_virt` 越界导致**递归缺页**。如果某张页表的物理地址落在了没做高半区映射的范围,VMM 访问它 → 缺页 → demand paging handler 试图 map → 又要 walk 页表 → 又访问那张表 → 又缺页……递归不收敛,最终 double fault、三重错误、机器重启。这种「一碰就重启」且栈里全是 `handle_pf` 的崩溃,先怀疑 `phys_to_virt` 走到了未映射区域。这一章在 boot 期(所有物理内存都高半区映射了)不发作,但要心里有数:demand paging + 自举访问是个可能递归的组合,以后缩小高半区映射范围时这里是雷区。

## 验证

VMM 的逻辑(4 级索引提取、页表 walk、map/unmap/translate、缺表分配)大半能在 host 上镜像测。[test_vmm.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_vmm.cpp) 把这些逻辑抄了一份,`-O2` 编、`CINUX_HOST_TEST` 门控,测 index 提取对不对、walk 一条已知路径对不对、map 后 translate 是否一致、缺中间表时是否正确分配:

```cpp
// 大致覆盖:
// - PML4/PDPT/PD/PT 索引从虚拟地址正确提取
// - map 后 translate(virt) 返回挂上的 phys + offset
// - unmap 后 translate 返回 0(不在)
// - 中间表缺失时 map 自动建立(计数字数对得上)
```

跑它们:

```bash
ctest --test-dir build -R vmm --output-on-failure
```

但「真页表、真 CR3、demand paging 真触发」只有 QEMU 里验得了真。机内测 [test_vmm.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_vmm.cpp) 在 QEMU 里:调 `g_vmm.init`(读真 CR3)、`map` 一个地址后 `translate` 验证、主动触发一次未映射地址的访问看 demand paging 是否补上页(补上后那行 `[VMM] Demand-paged %p -> phys %p` 会打出来):

```bash
cmake --build build --target run-big-kernel-test
```

init 时会打印 `[VMM] Initialised, kernel PML4 at phys %p`,看到这行 + demand paging 的补页日志,就说明 VMM 走通了。这一章的验证难点是「页表正确性没法直接看」——你只能通过 translate 是否一致、访问是否成功来间接验证,所以那批 host 单测(焊死 walk 算法)和机内测(真跑一遍)缺一不可。

## 下一站

到这里,内核的内存子系统两块基石都就位了:PMM 管物理页的分配,VMM 管虚拟↔物理的映射,还顺手实现了 demand paging。内核现在有了主动控制地址空间的正经能力——013 那个 `map_mmio` hack 也终于有了它的替代者(这一章 framebuffer 仍走旧路,VMM 是为后面铺的路)。

但你会发现一个粒度上的缺口:PMM 和 VMM 的最小单位都是**一整页 4KB**。如果你想「分配 64 字节」存个结构体,得拿一整页——既浪费,又难管理(谁拥有这页?释放了其中 64 字节怎么办?)。内核需要一个小粒度的分配器,在页的基础上切块、回收,也就是堆(heap)。

下一站就是它:一个堆分配器,让内核能 `kmalloc(64)` / `kfree(...)`,在 VMM 给的页上做细粒度管理。那是内存子系统的第三块拼图,也是后面几乎所有运行时数据结构(动态数组、链表、缓存)的依赖。不过那是下一章的事了,我们先享受一下「内核掌控了虚拟地址空间」这个里程碑。

---

### 参考

- Intel SDM Vol.3(System Programming,4 级分页):PML4→PDPT→PD→PT 四级结构、每级 9 位索引(512 项)、页表项(64 位)的位布局(物理地址位 `[12..51]`、P/RW/U/PS/NX 等 flag)、`CR2`(存缺页地址)、`CR3`(存 PML4 物理基址)、`INVLPG`(作废单页 TLB)。本地 PDF `document/reference/intel/SDM-Vol3A-*.pdf`,可用 `pdf-reader` 搜 "4-Level Paging"/"Page-Fault Error Code" 复核。
- OSDev — [Paging](https://wiki.osdev.org/Paging):4 级页表结构、PTE 位含义、页表 walk 的社区参考实现。
- 015 章 · [给物理内存建账本:bitmap PMM](015-mm-pmm.md):VMM 的中间页表和 demand paging 都靠 PMM 的 `alloc_page` 提供物理页,两章是内存子系统的一对基石。
- 本 tag 源码:[vmm.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/vmm.hpp) / [vmm.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/vmm.cpp)、[paging_config.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/paging_config.hpp)、[paging.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/paging.hpp)(`PageEntry`/`flush_tlb`/`read_cr3`)、[exception_handlers.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/exception_handlers.cpp)(`handle_pf` demand paging)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp)(Step 8 `g_vmm.init`);测试 [test_vmm.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_vmm.cpp)(host 镜像)、[test_vmm.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_vmm.cpp)(QEMU 真 map/demand paging)。
