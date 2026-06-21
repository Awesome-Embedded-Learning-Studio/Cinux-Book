---
title: 018 · 给每个世界一套页表:地址空间
---

# 018 · 给每个世界一套页表:地址空间(AddressSpace)

> 上一章(017)我们让内核能 `new` / `delete` 了,但留了个没收口的细节:堆被硬编码映射在 `0xFFFF800000000000`——这个数哪来的?为什么是它?还有,016 章 VMM 的 `map` / `unmap` / `translate` 都带一个看起来多余的 `pml4` 参数,当时说「为以后给每个进程一套独立页表留的」。这一章把这两件事一起收口:我们写一个 `AddressSpace`,把「一个虚拟地址空间」抽象成一个拥有自己 PML4 的对象——构造时拷一份内核的映射进来、用户态区域私有、能切换(CR3)、能销毁回收。从此「给某个进程一套独立页表」不再是一句承诺,而是一个能 `new` 出来的类型。顺带,`0xFFFF800000000000` 的谜底也解开了。

## 这一章我们要点亮什么

核心是一件:把**虚拟地址空间**变成一等公民——一个可以创建、切换、销毁的对象 `AddressSpace`。

具体说,一个 `AddressSpace` 实例:
- **拥有自己的一套页表根**(一张独立的 PML4),和内核的、和别的实例的都不一样。
- **看得见内核**:它的 PML4 里,内核那一半(PML4[256..511])是从内核页表拷过来的,所以无论切到哪个地址空间,内核映射始终在——否则一切换就找不到内核代码了。
- **用户那一半私有**(PML4[0..255]):每个空间自己往里映射,互不可见。这正是「进程隔离」的物理基础。
- **能切换**:`activate()` 把自己的 PML4 写进 CR3,从此 CPU 按这套页表走地址。
- **能销毁**:析构时把用户半区的页表子树(连同里面的数据页)全回收还给 PMM,内核半区不动。

合起来,这一章给了内核「造一个隔离的虚拟世界、钻进去、再拆掉」的能力。016 留的 `pml4` 参数,这一章真正用上了。

但要把期望放正:这一章**只**交付「地址空间」这个基础设施。它造出来是给谁用的?——给进程。可进程(进程结构、调度、上下文切换)是下一章(019)的事。所以在 018,`AddressSpace` 在生产路径里其实只做了一件事(存下内核 PML4),实例只活在测试里——它是一块「铺好、测好、等进程来用」的地基。

## 为什么现在需要它

先解开那个数字。`0xFFFF800000000000` 看着像拍脑袋选的,其实它是 x86-64 虚拟地址空间里一个有确切含义的位置。回忆 016:虚拟地址的 [39..47] 位是 PML4 的索引(9 位,共 512 项)。把这 9 位单独看:

```text
   PML4 索引 = (virt >> 39) & 0x1FF        // 0..511

   0xFFFF800000000000  →  PML4[256]   ← 堆基址(017): 内核半区的第一个表项
   0xFFFFFFFF80000000  →  PML4[511]   ← KERNEL_VMA / phys_to_virt: 内核半区的最后一个表项
   0x0000000020000000  →  PML4[  0]   ← 用户态地址: 用户半区的起点
   0x00007FFFFFFFFFFF  →  PML4[255]   ← 用户态地址: 用户半区的顶
```

`0xFFFF800000000000` 的 PML4 索引正好是 **256**。这不是巧合:x86-64 的虚拟地址是「规范地址(canonical)」,bit 47 是分水岭——bit 47 为 0 是低半区(用户),为 1 是高半区(内核),且高位必须按 bit 47 符号扩展。PML4 索引 256 正是 bit 47 第一次为 1 的那个表项,也就是**内核半区的入口**。所以 017 把堆放在 `0xFFFF800000000000`,等于放在「内核半区的第 0 项」——它待在所有地址空间都会共享的那一半里。这就是那个硬编码数字的全部含义:它在内核半区,因此天然在(未来的)每个地址空间里都可见。

这就引出了这一章的动机。到目前为止,内核只有「一套」页表——bootloader / VMM 建的那套,所有代码都在它上面跑。PMM、VMM、堆,全在内核半区。可是将来的进程需要**各自的**虚拟地址空间:进程 A 看到的 `0x400000` 不能是进程 B 看到的同一个物理页,否则谈不上隔离。要做到这点,每个进程得有自己的 PML4,自己的用户半区;但内核只有一份,不能给每个进程都拷一份完整的内核映射——那既贵又没必要。

经典的解法就是「**共享内核半区**」:每个地址空间自己有一张 PML4,其中内核半区那 256 项(PML4[256..511])指向**同一套**内核下级页表(PDPT/PD/PT),用户半区(PML4[0..255])各自独立。这样内核映射在所有空间里都一致可见(共享),用户映射各自隔离(私有),开销只是一张 PML4(4 KB)加用户半区的页表。`AddressSpace` 就是这个解法的落地。

所以 018 紧跟 017:017 让内核能动态分配了,018 把「地址空间」这个抽象立起来,为进程铺最后一层地基。016 的 `pml4` 参数,到这里终于有了调用者。

## 设计图

`AddressSpace` 的核心是「每个实例一张独立 PML4,内核半区共享、用户半区私有」。

```text
   内核 PML4(init_kernel 时从 CR3 存下)         某个 AddressSpace 实例的 PML4
   ┌──────────────────────┐                      ┌──────────────────────┐
   │ [0]   用户 ...        │  ← 私有, 各实例不同   │ [0]   用户(空, 自己 map)│
   │ ...                   │                      │ ...                   │
   │ [255] 用户顶           │                      │ [255]                 │
   ├──────────────────────┤                      ├──────────────────────┤
   │ [256] ──┐ 内核 PDPT    │   构造时:逐项拷贝     │ [256] ──┐ 指向【同一套】│
   │ ...     │  (heap 等)   │  ────────────────►   │ ...     │  内核下级页表 │
   │ [511] ──┘ 内核镜像/VMA  │   (浅拷贝, 共享引用)  │ [511] ──┘              │
   └──────────────────────┘                      └──────────────────────┘
        ▲                                              ▲
   kernel_pml4_ (static)                         pml4_phys_ (每实例)
   g_vmm 默认用这套                              activate() → 写进 CR3

   关键:PML4[256..511] 是「浅拷贝」——新 PML4 的这些项指向内核那套
   PDPT/PD/PT,所以内核映射在所有空间共享;用户半区各自独立的下级页表。
```

生命周期是这五步:

```text
   启动:  AddressSpace::init_kernel()           ← read_cr3() 存 kernel_pml4_
            (VMM init 之后, 任何 AS 构造之前; main.cpp Step 9)

   构造:  AddressSpace as;
            alloc 一页做 PML4 → 512 项清零 → PML4[256..511] = kernel_pml4_ 的对应项

   使用:  as.map(virt, phys, flags)   → g_vmm.map(..., &as.pml4_phys_)   // 以本空间为根
          as.translate(virt)          → g_vmm.translate(..., &as.pml4_phys_)
          as.activate()               → write_cr3(as.pml4_phys_)          // 切过去

   销毁:  ~AddressSpace()
            遍历 PML4[0..255], 每个 present 项 → free_subtree(回收 PDPT/PD/PT 子树 + 数据页)
            free_page(PML4 本身)
            内核半区[256..511] 一律不动(共享, 归内核)
```

## 代码路线

### init_kernel:启动时存下内核 PML4

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

### 构造:新 PML4 + 拷贝内核半区

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

### map/unmap/translate:透传 VMM,以本空间为根

这三个操作几乎不写新逻辑,全透传给 VMM,区别只在「用谁的 PML4 当根」:

```cpp
bool     AddressSpace::map(uint64_t v, uint64_t p, uint64_t f)       { return g_vmm.map(v, p, f, &pml4_phys_); }
void     AddressSpace::unmap(uint64_t v)                             { g_vmm.unmap(v, &pml4_phys_); }
uint64_t AddressSpace::translate(uint64_t v)                        { return g_vmm.translate(v, &pml4_phys_); }
```

看那个第四参数 `&pml4_phys_`——它就是 016 章 `VMM::map(virt, phys, flags, uint64_t* pml4 = nullptr)` 里那个「为以后留的」参数。当时默认 `nullptr` 用内核自己的 PML4;现在 `AddressSpace` 把自己那套 PML4 的地址传进去,VMM 的整条 walk 逻辑(缺表建表、末级挂页、flush TLB)就全在**这个空间的页表**上发生,而不是内核的。

所以这一章没重写 walk,而是复用了 016 的成果。这也反过来说明 016 那个参数设计得对:把「根」参数化,一套 walk 服务所有地址空间。

### activate:切 CR3

有了独立的页表,还得能让 CPU「用」上它。`activate()` 一行:

```cpp
void AddressSpace::activate() {
    cinux::arch::write_cr3(pml4_phys_);           // 把本空间的 PML4 写进 CR3
}
```

`write_cr3` 一执行,CPU 立刻换用新的 PML4 走地址,TLB 隐式刷新。从这一刻起,所有地址解析都走本空间的页表——用户半区是本空间私有的,内核半区因为拷贝过所以照常可见。

但「切过去」之后什么时候「切回来」,是调用者的责任——这一章的 `activate` 不负责自动恢复。测试里凡 `activate` 过的,都紧接着 `write_cr3(kernel_pml4)` 把 CR3 改回内核(见验证节 Test 8/9)。真正「切来切去」的调度逻辑,要等下一章的进程。

### 析构:只回收用户半区子树,内核半区不动

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

### 拷贝禁、移动允:物理页独占所有权

`AddressSpace` 持有真实的物理页(PML4 + 用户半区子树),所有权是独占的。所以它禁拷贝、允移动:

```cpp
AddressSpace(const AddressSpace&) = delete;             // 禁拷贝
AddressSpace& operator=(const AddressSpace&) = delete;
AddressSpace(AddressSpace&& other) noexcept;            // 允移动
AddressSpace& operator=(AddressSpace&& other) noexcept;
```

拷贝一旦被允许,两个对象会以为自己各拥有一份 PML4,析构时同一批物理页被 free 两次——经典的 double-free。禁掉拷贝就从根上杜绝。移动则把 `pml4_phys_` 从源对象「搬」到目标对象、源置 0(这样源析构时 `pml4_phys_ == 0` 直接跳过回收,不会误 free)。移动赋值还多一步:先把目标自己原有的资源释放掉,再接管别人的——标准的「释放旧的、接管新的、源置空」三段式。

这套「物理资源型 = 禁拷贝 + 五法则移动」是内核里管理独占资源的常见姿势,堆的 `BlockHeader`、PMM 的页,本质上都是同类问题。

## 调试现场

这一章也没有 notes 文件,但 `AddressSpace` 有三个「错一步就全机重启」的隐患,值得当调试现场。

**一是 `init_kernel` 没在构造之前调,或者调早了。** `kernel_pml4_` 启动前是 0。如果第一个 `AddressSpace` 构造时 `kernel_pml4_` 还是 0(忘了在 main 里调 `init_kernel`,或者调的时机早于 VMM 把 CR3 换成真内核页表),构造函数第三步就从地址 0 那里「拷内核半区」——`phys_to_virt(0)` 读到的是物理 0 处的随机内容,拷进新 PML4 的全是垃圾。接着 `activate` 一切,CR3 指向一张内核半区全是垃圾的 PML4,CPU 立刻找不到内核代码,三重错误、重启。症状是「一构造/切换地址空间就重启」,且串口可能在重启前来得及打半行乱码。根因:确保 `init_kernel()` 在 VMM init 之后、首个 AS 之前调用,且 `kernel_pml4()` 读出来非 0。测试里 Test 1(`test_init_kernel_pml4`)就是专门守这条——`TEST_ASSERT_NE(AddressSpace::kernel_pml4(), 0)`。

**二是 `activate` 切了 CR3 之后没切回来,空间就被析构了。** `activate` 把 CR3 换成某个 AS 的 PML4。如果这个 AS 随后析构(比如是个局部变量,出作用域),它的 PML4 及用户半区被 free——可此刻 CPU 还在用这张 PML4 走地址!于是 CPU 走在一张正在被回收的页表上,访问到刚被 free 的页表页 → 内容变垃圾 → 崩。规矩:`activate` 之后、析构之前,必须 `write_cr3(kernel_pml4)` 切回内核。测试里 Test 8、Test 9 都老老实实这么干——`activate` 完,先验完要验的,再 `write_cr3(saved_pml4)` 恢复,然后才让 AS 出作用域析构。真正「切来切去不手动恢复」要等下一章进程切换,那里有调度器管这事。

**三是析构的循环边界写错,扫进了内核半区。** 前面讲过:析构只扫 `PML4[0..255]`。如果误写成扫到 511,`free_subtree` 就会顺着 PML4[256..511] 把内核共享的那套 PDPT/PD/PT 当成「本空间私有」全 free 掉。内核页表没了,下一次任何地址解析(包括中断返回)都炸,而且炸得毫无预兆——因为 free 的时候还好好的,要等 CPU 下次需要走内核映射才暴露。这种「析构完一会儿才崩」的症状,先怀疑是不是回收越界扫进了共享区。守这条的测试是 Test 11(`test_destroy_no_kernel_corruption`):构造一个 AS、映射点东西、析构,然后验 `kernel_pml4()` 没变——确保析构没碰坏内核。

## 验证

地址空间的逻辑(PML4 分配、清零、内核半区拷贝、用户半区隔离、析构回收),核心能在 host 上镜像测。[test_address_space.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_address_space.cpp) 用一个 `MockPMM` + `TestVMM` 把 `AddressSpace` 的行为抄了一份:host 端分配伪造的「物理页」、用简化的 walk 验证隔离。它专门盯几条:构造会分配一张 PML4、构造后用户半区(PML4[0..255])全 0、内核半区(PML4[256..511])从 kernel PML4 拷过来了、两个实例拿到不同的 PML4 物理地址、析构把 PML4 页还回去(无用户映射时只回收 PML4 本身):

```bash
ctest --test-dir build -R address_space --output-on-failure
```

host 侧受限于「没法真改 CR3、没法真缺页」,主要验证页表结构和拷贝/回收的记账;真正的隔离、真正的 CR3 切换,还得 QEMU。

「真 PMM、真 VMM、真 CR3」在 QEMU 里验。[test_address_space.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_address_space.cpp) 在机内跑 11 个场景:init_kernel 存了非 0 的内核 PML4、构造出与内核不同的 PML4、两个实例根不同、单空间 map/translate/unmap、translate 未映射返回 0、**跨空间隔离**(Test 7:在 AS#1 映射,AS#2 `translate` 同一地址返回 0)、**activate 改变 CR3**(Test 8:切完后 `read_cr3() == as.pml4_phys()` 且与切前不同,随后恢复)、activate 后 translate 仍对、同空间映射两页、析构不损坏内核映射:

```bash
cmake --build build --target run-big-kernel-test
```

`init_kernel` 会打 `[AS] Kernel PML4 saved at phys ...`,test section `AddressSpace Tests (018)` 全过、末尾 `ALL TESTS PASSED`,就说明地址空间这套在真硬件语义下成立。尤其 Test 7 的跨空间隔离,是这一章的里程碑要求——两个空间对同一虚拟地址看到不同结果,进程隔离的物理基础就在这一步被焊实。

## 下一站

内存子系统至此收口:PMM 管物理页,VMM 管虚拟↔物理映射(还给了 demand paging),堆在页上做细粒度分配,`AddressSpace` 把「虚拟地址空间」抽象成可创建/切换/销毁的对象。`0xFFFF800000000000` 的谜底解开了——它是 PML4[256],内核半区的入口,所以天然在所有地址空间里可见。016 留的 `pml4` 参数,也终于有了 `AddressSpace` 这个正经调用者。

但你会发现:`AddressSpace` 在生产路径里只做了 `init_kernel` 一件事,一个实例都没造。它是一块铺好、测好、却还没人住的地基。谁来住?——进程。一个进程需要自己的地址空间、自己的执行流、能在多个进程间切换。

下一站(019)就是把它接上:进程结构、调度器、上下文切换。`AddressSpace` 会在那里第一次被真正「用起来」——每个进程挂一个自己的地址空间,调度器在切换进程时 `activate` 对应的空间。不过那是下一章的事,我们先享受一下「内核有了地址空间抽象」这个里程碑。

---

### 参考

- Intel SDM Vol.3(System Programming):4 级分页(PML4→PDPT→PD→PT)、`CR3`(PML4 物理基址)、**规范地址(canonical address)**——bit 47 为低/高半区分界、高位须符号扩展,这正是「PML4[256] 为内核半区入口」的由来。本地 PDF `document/reference/intel/SDM-Vol3A-*.pdf`,可用 `pdf-reader` 搜 "canonical" / "4-Level Paging" 复核。
- 016 章 · [把物理页挂进虚拟地址:VMM](016-mm-vmm.md):`AddressSpace` 的 map/unmap/translate 透传的就是 VMM 的 `pml4` 参数,`phys_to_virt` 自举换算也来自这一章。
- 017 章 · [在页上切块:内核堆分配器](017-mm-heap.md):堆基址 `0xFFFF800000000000`(= PML4[256])的来历,这一章给出了它「为什么」的答案。
- 本 tag 源码:[address_space.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/address_space.hpp) / [address_space.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/address_space.cpp)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp)(Step 9 `AddressSpace::init_kernel()`,生产路径只此一句);测试 [test_address_space.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_address_space.cpp)(host 镜像,MockPMM + TestVMM)、[test_address_space.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_address_space.cpp)(QEMU 11 场景,含跨空间隔离与 CR3 切换)。
