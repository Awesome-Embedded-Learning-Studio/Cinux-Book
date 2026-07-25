---
title: 02 · 代码路线:从 init_kernel 到析构回收
---

# 代码路线:从 init_kernel 到析构回收

## init_kernel:启动时存下内核 PML4

一切的前提,是知道「内核那套页表根在哪」。[address_space.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/address_space.cpp) 用一个静态成员存它:

```cpp
uint64_t AddressSpace::kernel_pml4_ = 0;          // 静态, 启动前为 0

void AddressSpace::init_kernel() {
    kernel_pml4_ = cinux::arch::read_cr3();        // CR3 就是当前 PML4 的物理基址
    cinux::lib::kprintf("[AS] Kernel PML4 saved at phys %p\n", (void*)kernel_pml4_);
}
```

`CR3` 寄存器里装的就是「当前生效的 PML4 的物理地址」。这一章运行到这里时,CR3 指向的是 016 章 VMM 建好的那套内核页表——`init_kernel` 把它记下来,作为「内核半区的源头」。后面每个 `AddressSpace` 构造时,都要从这套 PML4 里把内核半区拷过去。

调用时机在 [main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp) Step 9,卡在两个依赖之间:

```cpp
cinux::mm::g_vmm.init();                          // Step 8: VMM 把内核页表建好
cinux::mm::AddressSpace::init_kernel();           // Step 9: 存下内核 PML4
cinux::mm::g_heap.init(HEAP_VIRT_BASE, HEAP_INITIAL_SIZE);  // Step 10: 堆
```

必须在 VMM init **之后**(这时 CR3 才指向真正的内核页表,而不是 bootloader 的临时表),且在任何 `AddressSpace` 实例构造**之前**(构造要读 `kernel_pml4_`,它得先有值)。顺序错了——要么存的是临时表、要么拷的是 0——后面调试现场讲后果。

## 构造:新 PML4 + 拷贝内核半区

构造函数是这一章的灵魂:

```cpp
AddressSpace::AddressSpace() {
    pml4_phys_ = g_pmm.alloc_page();              // 1. 要一页做自己的 PML4
    if (pml4_phys_ == 0) { kprintf("[AS] FATAL: failed to allocate PML4 page\n"); return; }

    auto* pml4 = phys_to_virt(pml4_phys_);
    for (uint32_t i = 0; i < PT_ENTRIES; i++)     // 2. 512 项全清零
        pml4[i].raw = 0;

    auto* kern_pml4 = phys_to_virt(kernel_pml4_);
    for (uint32_t i = USER_PML4_END; i < PT_ENTRIES; i++)   // 3. PML4[256..511] 拷过来
        pml4[i].raw = kern_pml4[i].raw;
}
```

三步,步步有讲究。第一步从 PMM 要一页——和 016「中间页表也是一页物理内存」一样,PML4 本身就是一张 4 KB 的表(512 个 8 字节项)。要不到就打 FATAL、`pml4_phys_` 留 0(析构会据此跳过回收,见后)。

第二步**全清零**,这又是 016 那条铁律:不清零,残留位会被 `is_present()` 当成「这级映射在」,walk 跟着野地址飞。一张全新的 PML4 必须从「全 0」起步。

第三步是关键:`USER_PML4_END = 256`,所以 `for (i = 256; i < 512; i++) pml4[i] = kern_pml4[i]`——把内核半区那 256 项**逐项拷贝**过来。注意这是**浅拷贝**:拷的是 PML4 项本身(一个 8 字节整数,里面装着「指向某张 PDPT 的物理地址 + flag」),没有递归拷贝下级表。于是新空间的 PML4[256..511] 和内核 PML4[256..511] **指向同一批**内核 PDPT/PD/PT——内核映射就此被「共享引用」进新空间,而不是复制一份。这正是这套设计又便宜又正确的诀窍:内核映射只有一份,所有空间共享;以后内核在高半区新映射了什么(比如堆扩容),所有空间立刻能看见,因为它们看的是同一套下级表。

`phys_to_virt` 还是 016 那个自举换算——页表存在物理内存里,内核只能访问虚拟地址,所以 `phys + KERNEL_VMA`(`0xFFFFFFFF80000000`)换算回去读。这一章 `KERNEL_VMA` 和 016 完全一致,没有新花样。

## map/unmap/translate:透传 VMM,以本空间为根

这三个操作几乎不写新逻辑,全透传给 VMM,区别只在「用谁的 PML4 当根」:

```cpp
bool     AddressSpace::map(uint64_t v, uint64_t p, uint64_t f)       { return g_vmm.map(v, p, f, &pml4_phys_); }
void     AddressSpace::unmap(uint64_t v)                             { g_vmm.unmap(v, &pml4_phys_); }
uint64_t AddressSpace::translate(uint64_t v)                        { return g_vmm.translate(v, &pml4_phys_); }
```

看那个第四参数 `&pml4_phys_`——它就是 016 章 `VMM::map(virt, phys, flags, uint64_t* pml4 = nullptr)` 里那个「为以后留的」参数。当时默认 `nullptr` 用内核自己的 PML4;现在 `AddressSpace` 把自己那套 PML4 的地址传进去,VMM 的整条 walk 逻辑(缺表建表、末级挂页、flush TLB)就全在**这个空间的页表**上发生,而不是内核的。

所以这一章没重写 walk,而是复用了 016 的成果。这也反过来说明 016 那个参数设计得对:把「根」参数化,一套 walk 服务所有地址空间。

## activate:切 CR3

有了独立的页表,还得能让 CPU「用」上它。`activate()` 一行:

```cpp
void AddressSpace::activate() {
    cinux::arch::write_cr3(pml4_phys_);           // 把本空间的 PML4 写进 CR3
}
```

`write_cr3` 一执行,CPU 立刻换用新的 PML4 走地址,TLB 隐式刷新。从这一刻起,所有地址解析都走本空间的页表——用户半区是本空间私有的,内核半区因为拷贝过所以照常可见。

但「切过去」之后什么时候「切回来」,是调用者的责任——这一章的 `activate` 不负责自动恢复。测试里凡 `activate` 过的,都紧接着 `write_cr3(kernel_pml4)` 把 CR3 改回内核(见验证节 Test 8/9)。真正「切来切去」的调度逻辑,要等下一章的进程。

## 析构:只回收用户半区子树,内核半区不动

析构是这一章最容易写错的一段。目标:把这个空间**独占**的物理页(它的用户半区页表 + 用户半区数据页 + 它自己的 PML4)全还给 PMM,但**绝不能**碰内核半区——那是共享的,归内核。

```cpp
AddressSpace::~AddressSpace() {
    if (pml4_phys_ == 0) return;                   // 没分配成功 / 被 move 走了 → 跳过

    auto* pml4 = phys_to_virt(pml4_phys_);
    for (uint32_t i = USER_PML4_START; i < USER_PML4_END; i++)   // 只遍历 PML4[0..255]!
        if (pml4[i].is_present())
            free_subtree(pml4[i].phys_addr(), LEVEL_PDPT);       // 回收用户半区子树
    g_pmm.free_page(pml4_phys_);                   // 最后回收 PML4 本身
    pml4_phys_ = 0;
}
```

注意循环边界是 `USER_PML4_START..USER_PML4_END`,即 `0..256`——**只扫用户半区**。PML4[256..511] 那些指向共享内核表的项,一个都不动。要是手滑写成 `i < PT_ENTRIES`(扫到 511),就会把内核的 PDPT/PD/PT 当成「这个空间的」给 free 了——内核页表瞬间崩塌,全机重启。这条边界是析构的命门。

`free_subtree` 递归回收一棵子树:

```cpp
void AddressSpace::free_subtree(uint64_t table_phys, int level) {
    auto* table = phys_to_virt(table_phys);
    for (uint32_t i = 0; i < PT_ENTRIES; i++) {
        if (!table[i].is_present()) continue;
        if (level > LEVEL_PT)                      // 还没到 PT → 先递归回收下级表
            free_subtree(table[i].phys_addr(), level - 1);
        g_pmm.free_page(table[i].phys_addr());     // 回收本项指向的那一页
    }
}
```

`level` 从 `LEVEL_PDPT(3)` 开始,递减到 `LEVEL_PT(1)`。每到一个非 PT 层级,先递归进它的下级表;无论哪层,最后都 `free_page` 掉本项指向的页。这里有个要如实说的细节:**到了 PT 层(level==1),递归停了(PT 下面没有更深的表),但 `free_page` 照样执行**——也就是说,PT 项指向的**数据页**也会被回收。

这背后藏着一个所有权假设:用户半区里的每一页(页表页也好、数据页也好)都被当成「这个地址空间独占」,析构时一锅端。这对「进程退出、回收它全部私有内存」是对的;但它也意味着——**别往用户半区里映射「还要被别处引用的共享页」**,否则这个空间一析构,就会把那页从别人脚下抽走。018 还没有进程,没人这么用,所以不发作;但这是这套析构埋下的规则,以后做共享内存 / COW 时这里是雷区。源码注释里那句「PT 项指向的数据页不属于地址空间基础设施」半是提醒、半是和实际行为有点张力——以代码为准:它确实 free 了数据页。

## 拷贝禁、移动允:物理页独占所有权

`AddressSpace` 持有真实的物理页(PML4 + 用户半区子树),所有权是独占的。所以它禁拷贝、允移动:

```cpp
AddressSpace(const AddressSpace&) = delete;             // 禁拷贝
AddressSpace& operator=(const AddressSpace&) = delete;
AddressSpace(AddressSpace&& other) noexcept;            // 允移动
AddressSpace& operator=(AddressSpace&& other) noexcept;
```

拷贝一旦被允许,两个对象会以为自己各拥有一份 PML4,析构时同一批物理页被 free 两次——经典的 double-free。禁掉拷贝就从根上杜绝。移动则把 `pml4_phys_` 从源对象「搬」到目标对象、源置 0(这样源析构时 `pml4_phys_ == 0` 直接跳过回收,不会误 free)。移动赋值还多一步:先把目标自己原有的资源释放掉,再接管别人的——标准的「释放旧的、接管新的、源置空」三段式。

这套「物理资源型 = 禁拷贝 + 五法则移动」是内核里管理独占资源的常见姿势,堆的 `BlockHeader`、PMM 的页,本质上都是同类问题。
