---
title: 082 · SysV 共享内存:让两个进程的虚拟地址指向同一块物理页
---

# 082 · SysV 共享内存:让两个进程的虚拟地址指向同一块物理页

> IPC 四件套走到这一章,前面 pipe、FIFO 已经把「字节流过内核缓冲」这条路趟通——写者一次 syscall 把字节拷进管子、读者再一次 syscall 拷出来,两次穿越。这一刀(SysV 共享内存,shm)更狠:**它根本不搬字节,它搬地址**。两个进程各自 shmat 一块登记好的物理页到自己地址空间,底下落到的是**同一批物理帧**——写者写一个字节、读者**无需任何 syscall** 就在自己的虚拟地址里看到新值。零拷贝、零 syscall,这就是「共享」的真义。
>
> 可这套机制藏着一颗定时炸弹:进程退出时,地址空间析构要遍历每个用户 PTE 把映射的物理页「还回去」。如果一个共享段还活着、别的进程还在用,析构绝不能把段里的页误放回 free pool——否则就是 use-after-free 的物理版。这条账怎么记平,是这章最值得拆开看的一刀。再补一个 shmdt 长度的真实踩坑(取段的 `page_count`、不是取合并后 VMA 的跨度),shm 的正确性骨架就齐了。
>
> A 档:punchline 是**两个地址空间真把同一物理页映射通**——一个 AS 写 magic、另一个 AS 读回来完全相等,外加 translate 验证两虚拟地址确实落到同一物理帧。6 例 ring0 端到端测兜底。诚实的边界先摆前头:这套测用「栈上 AddressSpace + 空壳 Task」模拟两进程,不是真用户态 libc 跑通;`ShmRegistry` 设计上可链 host 单测但目前没配(对照 FIFO 有);shmid_ds 是精简内核内形状,跟 glibc 互操作还差一截。

## 这章咱们要点亮什么

1. **价值先说清:不是搬字节,是搬地址**。pipe 是字节流过内核 buffer(两次 copy),shm 是页表直接共享物理页(零 copy + 无 syscall 可见)。你一眼分清「搬数据 vs 搬地址」,后面 mapcount 闭环才有动机。
2. **分层铁律:纯逻辑表 vs 物理页生命周期**。`ShmRegistry` 是固定 16 槽的纯逻辑表(key→segment,只管簿记 + nattach/marked 状态机,零 kernel-only 依赖),`sys_shm` 才是物理页生命周期层(alloc_pages/map/unmap/free_pages)。这跟 071 的 `FifoRegistry` 是一个模子,跟 081 tmpfs「纯逻辑层 vs boot I/O 层」是同一套切法。
3. **双计数器闭环:段自带 refcount 基线 1 防 teardown 误放**。一块共享物理页的生命计数要保证进程退出绝不把「段还在用」的页误放回 free pool。Book 源码里是 `pte_count`(多少用户 PTE 映射此页)+ `refcount`(所有权引用)双计数器,alloc 给段 refcount 基线 1、shmat 同时 inc 两者、teardown 走两级 test。
4. **shmdt 长度陷阱:取段 page_count,不取合并后 VMA 跨度**。两段背靠背映射会并成一个 VMA,shmdt 按 VMA 算长度会顺带拆邻居的页——必须用段的 `page_count` 做权威长度。
5. **诚实分层的好处:ShmRegistry 设计上可链 host 单测,目前是缺口**。这层纯逻辑零 kernel-only 依赖,设计上 host 可测(同 fifo.cpp);但当前 test_shm 6 例全走 syscall 层在 ring0 跑,registry 的纯逻辑只被顺带覆盖——对照 FIFO 有 `test_fifo.cpp`,shm 没配,这是对称缺口不是 bug。

## 两进程共享一页要解决什么:不是搬字节,是搬地址

先回顾 071 的 pipe 和 FIFO。它们的本质是「**内核 buffer 搬字节**」:写者 `sys_write` 一次 syscall,把用户态字节拷进内核管道缓冲;读者 `sys_read` 再一次 syscall,把字节从内核缓冲拷出来。两次用户态↔内核态 crossing,两次 memcpy。这是字节流的代价。

shm 这一刀完全不同。`shm.hpp` 的头注释把机制一句话讲透了:

```cpp
/**
 * Two or more processes share a physical page range: shmget() allocates a
 * contiguous run of physical pages and registers it under an int key; shmat()
 * maps that run into the caller's address space; shmdt() tears the mapping
 * down; shmctl(IPC_RMID) marks the segment for destruction (the pages are freed
 * once the last attachment goes away).
 */
```

([shm.hpp](../../../kernel/ipc/shm.hpp#L5-L9),有删节。)

四个 syscall 拆开看:

- `shmget(key, size, shmflg)`——申请一块连续物理页,登记到一个 int key 下,返回 shmid(表索引)。
- `shmat(shmid, addr, shmflg)`——把这块物理页映射进**调用方**的地址空间,返回虚拟地址。多个进程对同一 shmid 各自调 shmat,各自拿到的虚拟地址**可以不同**,但底下的物理帧是**同一批**。
- `shmdt(addr)`——拆掉自己地址空间里这块映射(物理页不动,别的进程还共享着)。
- `shmctl(shmid, IPC_RMID, ...)`——标记销毁(段页最后一次 detach 后才真回收)。

这套机制的价值一句话说清:**pipe 搬数据,shm 搬地址**。写者写完一个字节,读者在自己进程里读那个虚拟地址——**没有任何 syscall**——就拿到新值。这是零拷贝 + 零 syscall 可见的「真共享」,代价是没有任何同步(写者和读者谁先谁后,得自己用信号量/互斥锁协调,不在 IPC 这层管)。

这四个 syscall 在 Book 里是真注册的,不是 stub。看 `syscall.cpp`:

```cpp
syscall_register(SyscallNr::SYS_shmget, sys_shmget);
syscall_register(SyscallNr::SYS_shmat, sys_shmat);
syscall_register(SyscallNr::SYS_shmctl, sys_shmctl);
syscall_register(SyscallNr::SYS_shmdt, sys_shmdt);
```

([syscall.cpp](../../../kernel/arch/x86_64/syscall.cpp#L224-L227)。)syscall 号落在 `syscall_nums.hpp`:`SYS_shmget=29`、`SYS_shmat=30`、`SYS_shmctl=31`、`SYS_shmdt=67`(`syscall_nums.hpp:46-48` 和 `:64`)——这四个号跟 Linux x86_64 ABI 对齐,musl/glibc 直接能调到。

> 串一句 071:那卷的 pipe/FIFO 搬数据,这卷的 shm 搬地址,是同一条 IPC 路上的姊妹刀。071 的 `FifoRegistry`(名字→FIFO)跟这卷的 `ShmRegistry`(key→segment)是直系模子——固定 16 槽 + index-as-handle,下一节展开。

## ShmRegistry 表层:承 071 的模子,固定 16 槽 + index-as-handle

承 071 的「传输/应用不混」分层铁律,shm 也劈两层。先看纯逻辑层 `ShmRegistry`(`kernel/ipc/shm.{hpp,cpp}`)。

它是一张固定大小的内存表:`kShmRegistryMax = 16` 个槽(`shm.hpp:67`),每槽一个 `ShmSegment`(`shm.hpp:99-110`):

```cpp
struct ShmSegment {
    bool     used{false};
    int      key{0};                     // IPC_PRIVATE (0) or user-supplied key
    uint64_t phys_base{0};               // physical base of the page run
    uint64_t page_count{0};              // number of 4 KB pages mapped
    uint64_t size{0};                    // requested size in bytes (for IPC_STAT)
    uint16_t mode{0666};
    uint32_t nattach{0};                 // live shmat count
    uint32_t cpid{0};                    // creator tid
    uint32_t lpid{0};                    // last shmat / shmdt tid
    bool     marked_for_removal{false};  // IPC_RMID set; freed when nattach hits 0
};
```

([shm.hpp](../../../kernel/ipc/shm.hpp#L99-L110),有删节。)一把 `Spinlock` 串行化所有操作(`shm.hpp:206`)。registry 的全部 API 就这几件:`find_by_key`(按 key 查)、`add`(占个空槽)、`attach`(快照 + nattach++)、`detach`(nattach--,marked 且归零就返回 phys_base 给调用方 free)、`mark_removal`(IPC_RMID)、`stat`(IPC_STAT 填 shmid_ds)、`find_by_phys`(shmdt 用 phys 反查 shmid)。

关键证据在 `add` 的签名:

```cpp
cinux::lib::ErrorOr<int> add(int key, uint64_t size, uint64_t phys_base, uint64_t page_count,
                             uint16_t mode);
```

([shm.hpp](../../../kernel/ipc/shm.hpp#L143-L144)。)`phys_base` 是**入参**——调用方(syscall 层)已经 alloc 完物理页把基址传进来。registry 自己**从不碰 PMM**:不开页、不关页、不增减计数器。物理页生命周期全部下沉到 `kernel/syscall/sys_shm.cpp`。这就是分层:`ShmRegistry` 只管「这张表里有什么」,`sys_shm` 管「物理页是死是活」。

> 跟 071 是同一个模子。`FifoRegistry`(`fifo.hpp:96` 的 `class FifoRegistry` + `:99` 的 `instance()` 单例 + `:122` 的 `entries_` 固定表)是「名字→FIFO」的纯内存表;`ShmRegistry` 是「key→segment」的纯内存表。俩都是 `FIFO_REGISTRY_MAX=16` / `kShmRegistryMax=16` 的定长表(避开 `<map>`/`<string>`),都是 index-as-handle(shmid 就是表里第几个槽),都是一把 Spinlock 串行化。071 立了模子,这卷第三次复用。

这个分层的真好处:**`ShmRegistry` 全是表操作,零 kernel-only 依赖**(无 PMM、无 AddressSpace、无 kprintf——`shm.cpp:1-9` 头注释明说「no kprintf / no I/O, links into host unit tests like fifo.cpp」),所以这层若要 host 单测,设计上零摩擦。但(诚实标一句)目前 Book 没给它配 host 单测——`test/unit/` 无 `test_shm`、`test/CMakeLists.txt` 无对应行;这层纯逻辑当前只被 6 例 ring0 syscall 测顺带覆盖。对照看 `FifoRegistry`:`test/unit/test_fifo.cpp` 存在,并入了 ctest,有独立的 host 回归网。`ShmRegistry` 没有对称物——这是 **fifo↔shm 的对称缺口**,不是 bug。后续要补,设计上零摩擦(同 fifo.cpp 的链法)。

分层叙事本身是教学点。这卷的姊妹章 081 tmpfs 把「纯逻辑层(`tmpfs.cpp` 无 kprintf)vs boot I/O 层(`tmpfs_init.cpp`)」分 TU;071 把「传输(`Pipe`)不混应用(`FifoRegistry` 命名)」分清。shm 是同一套切法的第三次复用:纯逻辑表(`ShmRegistry`)管「key→segment 簿记」,syscall 层(`sys_shm.cpp`)管「物理页是死是活」。这是 Cinux 一以贯之的切法——**把可单测的纯逻辑跟 kernel-only 的副作用分开**,既让纯逻辑可独立验证,也让副作用集中在一条路径上好审查。

shmid=表索引(0..15)是教学简化。Linux 真 shmid 是 `inx + seq`(序列号编码),防一个 stale 句柄被快速复用后打到新段。Book 固定 16 槽 + index 即 handle,代价是 RMID 后槽位被复用时,stale shmid 会命中新段。`shm.hpp:24-26` 头注释把这个取舍明说了——「matches FifoRegistry's index-as-handle model」。代价也一并摆这儿:RMID 后槽位被复用,stale shmid 会命中新段,别只看省事那一面。

## mapcount 闭环:段自带 refcount 基线 1 防 teardown 误放

正确性核心来了。一块共享物理页的「生命计数」必须保证:进程退出时(地址空间析构 `free_subtree` 遍历每个用户 PTE 做 `pte_count_dec_and_test`),**绝不能把「段还在用」的页误放回 free pool**——否则就是 use-after-free 的物理版,写者写、读者读到的是被回收重分配给别人后的脏字节。

先讲清模型层(概念),再讲实现真相(代码)。

**概念层**:shmget alloc 给段一份基线引用 → 每次 shmat +1 → 每次 shmdt 或地址空间销毁 -1 → 只有 IPC_RMID 的显式 `free_pages` 才回收。基线引用保证 teardown 的减法永远跌不到 0,段页存活到最后一次 RMID。

> **一个 C++ 工程坑:mapcount 是旧词。** 老的 dev note 里这套机制叫「mapcount」——单计数器,alloc 置 1、shmat inc、teardown dec。Book 源码已经做了 **batch 3 拆分**:单 mapcount 被拆成 `pte_count`(多少用户 PTE 映射此页)+ `refcount`(所有权引用)双计数器。**别拿「mapcount」这个词去 grep 源码,找不到;也别拿着旧措辞去理解,会跟真值对不上。** 直接用「refcount=1 是段自有所有权基线、pte_count=0 是 PTE 映射数、shmat 同时 inc 两者」这套讲法。下面以 `pmm.cpp` 真值为准。

**实现真相——双计数器,以 pmm.cpp 为准。** `alloc_pages` 给段的每个页初始化(`pmm.cpp:227-231`):

```cpp
uint64_t n_pages = static_cast<uint64_t>(1) << order;
for (uint64_t i = 0; i < n_pages; ++i) {
    __atomic_store_n(&refcount_storage_[page + i], 1, __ATOMIC_RELAXED);
    __atomic_store_n(&pte_count_storage_[page + i], 0, __ATOMIC_RELAXED);
}
```

注意:**`refcount=1`(段自有的所有权基线)、`pte_count=0`(还没有任何 PTE 映射它)**。这就是「段自带基线 1」落地的真位置——在 refcount 上,不在 pte_count 上。

shmat 安装映射时,每页同时 inc 两个计数器(`sys_shm.cpp:201-202`):

```cpp
cinux::mm::g_pmm.pte_count_inc(p);
cinux::mm::g_pmm.refcount_inc(p);
```

所以一个 attach 后的段页:`refcount = 2`(段基线 1 + attach 的 map-ownership 1),`pte_count = 1`(一个用户 PTE 映射它)。

teardown(进程退出 / shmdt)走 `pte_count_dec_and_test`,这是**两级 test**(`pmm.cpp:262-277`):

```cpp
bool PMM::pte_count_dec_and_test(uint64_t phys) {
    if (phys == 0 || phys / PAGE_SIZE >= total_pages_) {
        return false;  // unmanaged (device/IoPhys)
    }
    if (__atomic_sub_fetch(&pte_count_storage_[phys / PAGE_SIZE], 1, __ATOMIC_ACQ_REL) != 0) {
        return false;  // other PTEs still map it
    }
    return refcount_dec_and_test(phys);  // last PTE gone -> drop ownership ref (maybe free)
}
```

第一级:`pte_count` 先减 1,不归零就 return false(别的 PTE 还映射着,页当然不放)。只有 `pte_count` 跌到 0(这个地址空间里没人映射它了),才走第二级——再 `refcount_dec_and_test` 减一个 ownership ref:如果 refcount 也归 0,页才真放回 buddy。

**这就是「段页存活到 RMID」的代码根**。一个 attach 的段页 refcount=2。shmdt 时:pte_count 从 1 减到 0(这个 AS 不映射了)→ 再 dec refcount 从 2 到 1 → **1 不归零,页不放**。段自有的那份 refcount=1 还在撑着——只要没显式 IPC_RMID,页永远活着。哪怕所有 attach 都退出了(pte_count 全归零),refcount 仍 ≥ 1,页照样存活。teardown 的减法永远跌不到 0。

真释放路径:`IPC_RMID` 走 `mark_removal` → 最后一次 `detach` 返回 phys_base → syscall 层显式 `free_pages`(`sys_shm.cpp:258-260`、`sys_shm.cpp:279-281`)。`free_pages` 内部调 `free_page` → `buddy_.free` 把整块回收。这才是段页真回家的唯一路径。

> 串一句 fork CoW:`fork.cpp:66` 和 `:104` 父子共享页时也调 `pte_count_inc`——同一套 PTE 计数账的姊妹机制。fork 那卷把页拆给父子共享(写时复制),这卷把页拆给任意进程共享(显式 attach),底下都是「多个 PTE 指向同一物理页,teardown 只减 pte_count、不误放」。这是 Cinux 一以贯之的引用计数账。

**源码注释的措辞出入,这里得讲清。** 真正漂移的只有两处头注释:`shm.hpp:18-22` 和 `sys_shm.cpp:12-15`,它们把 alloc 基线说成「`pte_count` 置 1」:

```cpp
 * pte_count model (mirrors fork CoW, kernel/proc/fork.cpp): alloc_pages sets
 * each page to 1 (the segment's own reference); shmat adds +1 per page; ...
```

([sys_shm.cpp](../../../kernel/syscall/sys_shm.cpp#L12-L15),有删节。)这跟 `pmm.cpp:229-230` 的真值(`refcount=1 / pte_count=0`)对不上。措辞是历史遗留——旧实现是单 mapcount,注释写「置 1」是对的;batch 3 拆成双计数器后,基线挪到了 refcount 上,这两处头注释没同步改。**讲教程以 `pmm.cpp` 为真相**:`refcount=1` 是基线、`pte_count=0` 起步、shmat 同时 inc 两者。运行时正确性靠 `pte_count_dec_and_test` 的两级 test 守住(段页存活),注释措辞模糊但不影响行为。这种「注释跟代码漂移」是工程里常见的债,记一笔省得你拿着头注释去 grep `pte_count = 1` 找到对不上。

> grep 时认准这两处**头注释**(`shm.hpp:18-22` 和 `sys_shm.cpp:12-15`),别误伤同文件 `sys_shm.cpp:182-185` 的**安装注释**——那段写的是「map-ownership ref (refcount); the segment's own ref (alloc baseline=1)」,明确把基线归到 refcount,跟 `pmm.cpp` 真值一致,是 batch 3 重写过的正确版本。漂移只在头注释那一处,别拿对的注释去印证错的注释。

## shmdt 长度陷阱:取段 page_count,不取合并后 VMA 跨度

真坑,有专门回归测兜。这一节是个「设计决策的根因链」,咱们顺着讲。

`shmdt(addr)` 要算「拆多少页」。一个直觉的做法是查这个 addr 落在哪个 VMA、用 `vma->end - vma->start` 当长度。**这个直觉是错的**,原因是一条 VMA 合并规则。

VMA store 插入新映射时会合并相邻同标志、且**无 backing**(file_offset/backing 元数据)的 VMA(`vma.cpp:119-122`):

```cpp
const bool merge_prev = (prev != nullptr) && (prev->end == start) && (prev->flags == flags) &&
                        prev->backing == nullptr;
const bool merge_next =
    (cur != nullptr) && (cur->start == end) && (cur->flags == flags) && cur->backing == nullptr;
```

两个背靠背的 SHM 映射正好全中:**它们的 flags 完全一样**(都是 `Read|Shared`,可写的话加 `Write`)、`backing == nullptr`、地址相邻——结果被并成一个 VMA,跨度 = 两段之和。如果按 VMA 算 unmap 长度,shmdt 第一个段会顺带拆掉第二个段的页。

> 串一句:为什么 SHM 映射不带 `Anonymous` 位?看 `sys_shm.cpp:171-175` 的 VmaFlags:

```cpp
using cinux::mm::VmaFlags;
VmaFlags vf = VmaFlags::Read | VmaFlags::Shared;
if (!readonly) {
    vf |= VmaFlags::Write;
}
```

刻意只设 Read|Shared(+Write),**不带 Anonymous 位**。因为 SHM 段背后是**已 alloc 的真实物理页,eager map**(shmat 时立刻把每页装进页表),**不走缺页/demand-paging 路径**。匿名 mmap 才带 Anonymous(写时分配);SHM 不带,这跟它的 eager 语义一致。但代价是:两个 SHM 映射 flags 全等 + 无 backing,正好满足合并条件。

**正解——用段自己的 `page_count` 算长度**(`sys_shm.cpp:228-253`):

```cpp
const uint64_t phys_base = task->addr_space->translate(addr);
if (phys_base == 0) {
    return -kEinval;
}
auto shmid_r = ShmRegistry::instance().find_by_phys(phys_base);
if (!shmid_r.ok()) {
    return -kEinval;
}
const int                     shmid = shmid_r.value();
const cinux::ipc::ShmSegment* seg   = ShmRegistry::instance().segment(shmid);
if (seg == nullptr || seg->page_count == 0) {
    return -kEinval;
}

const uint64_t pages = seg->page_count;
const uint64_t len   = pages * kPageSize;
for (uint64_t i = 0; i < pages; ++i) {
    const uint64_t v = addr + i * kPageSize;
    const uint64_t p = phys_base + i * kPageSize;
    task->addr_space->unmap(v);
    static_cast<void>(cinux::mm::g_pmm.pte_count_dec_and_test(p));
}

static_cast<void>(task->addr_space->vmas().remove(addr, addr + len));
```

([sys_shm.cpp](../../../kernel/syscall/sys_shm.cpp#L228-L253),有删节——省略了 detach 后的 free_pages 路径。)调用方 addr → `translate` 得 phys_base → `find_by_phys` 定位段 → **取段自己的 `page_count`** 算 `len = pages * 4096` → 逐页 unmap + `pte_count_dec_and_test` → `vmas().remove(addr, addr + len)`。`remove()` 戳洞,正确处理合并 VMA 的切分(把合并节点劈成两半,不会误删邻居)。

源码注释把这条决策讲得很直白(`sys_shm.cpp:221-227`):

> We deliberately do NOT use vma->start / vma->end for the length: vmas().insert() coalesces adjacent same-flags mappings, so two back-to-back SHM attachments in one address space share a single VMA and its [start,end) would span more than this segment. The segment's own page_count is the authoritative teardown length; remove() then punches a clean hole even out of a merged VMA.

**根因链一条讲完**:为什么 SHM 映射不带 Anonymous?背后是已 alloc 的真实物理页(eager map,不走缺页)。为什么两 SHM 映射会合并?合并条件只看 flags 相等 + 无 backing,SHM 恰好满足。为什么 shmdt 不能取 VMA 长度?合并后跨度大于单段。为什么取 `page_count` 就对?因为**段是分配的权威单元**(shmget 一次 alloc 整块),`remove()` 会从合并 VMA 戳干净洞。你能记住「权威长度来自段不是 VMA」这个真坑(footgun)。

> **调试五段标签——shmdt 长度踩坑回归**:
> - **症状**:两个相邻 SHM 段,shmdt 第一个后第二个页被踩、读出脏值或 #PF。
> - **根因**:shmdt 按 VMA 跨度算 unmap 长度,合并后 VMA 跨度 = 两段之和,第一个的 unmap 把第二个的页也拆了。
> - **定位**:`vma.cpp:119-122` 合并条件 + `sys_shm.cpp` 原 shmdt 路径取 VMA 长度。
> - **修复**:shmdt 改成 `translate(addr) → find_by_phys → seg->page_count` 取段自己的页数当权威长度,`remove()` 戳洞处理合并 VMA 切分。
> - **防复发**:`test_shm.cpp:277-319` 的 `test_adjacent_detach_preserves_peer` 专门锁——两段贴邻(a1=base、a2=base+4096)、写不同 magic(0x1111... / 0x2222...)、shmdt 第一段后断言 `translate(a1)==0`(已拆)且 `translate(a2)!=0`(还在)+ 读 a2 仍得 0x2222... 而非被踩。

## 验证

四层,合起来才够。每层验什么、为什么不验,都得讲清。

**第一层:host 单测——讲清有没有、为什么没有。** `ShmRegistry` 没配 host 单测——这层纯逻辑目前只被下面的 ring0 syscall 测顺带覆盖(缺口根因和对照 fifo 的细节上一节 ShmRegistry 那段讲过了,这里不重复)。

**第二层:kernel 测(`test_shm.cpp` 6 例,`run_shm_tests` 在 `main_test.cpp:1208` 无条件调用)。** 这是当前的主力回归网,全走 syscall 层在 ring0 跑。六例分别锁:

1. **round-trip 两地址空间同物理页 + 跨 CR3 写读**(`test_shm.cpp:80-133`,Test1)。机制证明双保险:先断言 `as1.translate(virt1) == as2.translate(virt2)`(两虚拟地址落到同一物理帧),再 `as1.activate()` + `user_write_u64` 写 magic 0x48454c4c4f53484d、`as2.activate()` + `user_read_u64` 读、断言读到的等于写的。这是「真共享」的端到端证据。
2. **IPC_STAT 报 size/nattch**(`test_shm.cpp:143-168`,Test2)。用 3 页段验 `shm_segsz` 和 `shm_nattch`(attach 前 0、attach 后 1)。
3. **RMID 后 shmat 失败 + 未 attach 的 RMID 立即释放**(`test_shm.cpp:178-205`,Test3)。两条都锁:marked 段再 shmat 返负(EINVAL)、nattach=0 的 RMID 立即回收(再 shmat 同 shmid 失败)。
4. **命名 key 的 create/reopen/EXCL/ENOENT 四态**(`test_shm.cpp:215-234`,Test4)。reopen 用 `2*kPageSize` 的 size 但返同 shmid——**size 在复用时被忽略**(段已存在,shmget 直接返原 shmid)。
5. **非法参数全拒**(`test_shm.cpp:244-264`,Test5)。size=0、假 shmid(0xDEAD)、未映射地址 shmdt、假 id IPC_STAT 全返负。
6. **相邻映射 VMA 合并的 detach 回归**(`test_shm.cpp:277-319`,Test6)。就是上一节那个真坑(footgun)的防复发测。

> **测试框架细节**:`big_kernel_test.h` 的 `RUN_TEST` 宏(`big_kernel_test.h:174-183`)**无 skip 概念**——记下 `_failed_before`、跑完 `fn()`、若 `tests_failed` 没涨就算 PASS(`tests_passed++`),失败时 `TEST_ASSERT` 宏(`big_kernel_test.h:133-141`)做 `tests_failed++`。`ASSERT_OK` 那种硬失败会 `cinux::io::io_outb(0xf4, 1)`(向端口 `0xf4` 写字节 1)让 QEMU `isa-debug-exit` 退出码 3(`big_kernel_test.h:158-168`)。所以「6 例 PASS」是真跑真断言,不是静默跳过。

**SMAP 细节**。ring0 读写用户映射页必须走 stac/clac 窗口——`test_shm.cpp:57-70` 封装了 `user_write_u64`/`user_read_u64`:

```cpp
void user_write_u64(uint64_t addr, uint64_t value) {
    cinux::arch::stac();
    *reinterpret_cast<volatile uint64_t*>(addr) = value;
    cinux::arch::clac();
}
```

([test_shm.cpp](../../../kernel/test/test_shm.cpp#L57-L62),`user_read_u64` 同款。)直接 ring0 读写用户映射页会 #PF(SMAP 挡),stac 临时打开用户访问、clac 关上。每个 AddressSpace 操作前先 `activate()` 换 CR3(`test_shm.cpp:112, 115`)、完事 `write_cr3(AddressSpace::kernel_pml4())` 回内核空间(`test_shm.cpp:119`)——否则栈上 AddressSpace 析构会拆当前 CR3 下的页表,把测试自己的页表拆了。这两个反直觉点 lab-082 让你亲自踩一遍。

**第三层:shell/用户态闭环——讲清目前没有。** musl/glibc 用户态 SHM demo 是 follow-up。当前 `shmid_ds`(`shm.hpp:81-87`)是精简内核内形状,只有 `shm_segsz/cpid/lpid/nattch/mode` 五字段,不是 Linux 全 `ipc_perm`(uid/gid)+ 时间戳(`shm_atime/dtime/ctime`)布局。真要跟 glibc 互操作得先拓宽这个结构体,这是诚实边界。

**第四层:全量 `run-kernel-test-all`。** 两腿(单核 + `-smp 2`)的 passed/failed 数字,**必须在 Book 工作树真跑后填**,不照抄源仓库 dev note 的数(那是源仓库的,Book 侧须独立验证)。`test_shm.cpp` 6 例都进 `big_kernel_test`,真跑后 passed 数应包含这 6 例。用户态真能用 shm 靠「syscall 真注册(`syscall.cpp:224-227`)+ ring0 测端到端通」两腿绿间接证明,不是 big_kernel_test 的直接断言——四层证据合力,任一单独都不够。

> **测试数字怎么填。** 写章节时若没真跑 `run-kernel-test-all`,passed 数字别照抄 dev note 的 1067——那是源仓库 worktree 的数,Book 侧须独立实跑。「教程即验证」不是口号,数字要么标「Book 实测」、要么留空待跑。本教程成稿时这一格留空,等你跑完 `cmake --build build --target run-kernel-test-all` 把两腿数字填进去。

## 这章没做的

- **musl/glibc 用户态 SHM demo**。`shmid_ds` 当前是精简内核内形状(`shm.hpp:81-87` 五字段),不是 Linux 全 `ipc_perm`(uid/gid)+ 时间戳(`shm_atime/dtime/ctime`)布局。真要跟 glibc 互操作得先拓宽这个结构体。这是用户态 demo 推迟的根因。
- **IPC_SET / IPC_INFO / SHM_LOCK 等高级 shmctl 命令**。`do_shmctl_kernel` 对非 STAT/RMID 直接返 `-ENOSYS`(`sys_shm.cpp:284-285`)。SHM_LOCK/UNLOCK(把段页钉在 RAM 不 swap)依赖 swap 子系统,Cinux 没有 swap,这两个命令永远不会做。
- **权限强制**。`IPC_CREAT` 命中既有段、shmat 的 `SHM_RDONLY` 与段 mode 的交互现仅记 mode 不强制检查(`sys_shm.cpp:137-138` 的 readonly 粗判),没有完整 uid/gid 检查。教学内核假设单用户/可信,不做权限强制。
- **SMP 下 `ShmRegistry::segment()` 快照与 detach/mark_removal 之间的 TOCTOU**。单线程测不触发,`shm.hpp:147-150` 注释明说「caller-supplied exclusion is assumed」(调用方假设排他)。真 SMP 多核同时 attach/detach 同一段有竞态,文档化为已知局限。
- **Linux 的 sequence number 防 stale 句柄复用**。教学内核固定表 + index-as-handle 不做,RMID 后槽位被复用时 stale shmid 会命中新段,代价已记(`shm.hpp:24-26`)。
- **真两进程(scheduler 循环)跑**。ring0 测用 `Scheduler::set_current` 装的空壳 Task(`test_shm.cpp:89-92, 95, 100`)、activate/CR3 切换手动(`test_shm.cpp:112, 115, 119`),不走调度器循环。所以「真用户态两进程通过 shm 通信」没被这套测覆盖,是 follow-up。真 SMP 多核 TLB shootdown 这条链 shm 也没走(deferred-free 变体 `pte_count_dec_and_test_no_free` 在 drain kthread 路径别处用)。
- **buddy 尾页的 refcount 残留**。`alloc_pages` 向上取整到 2 的幂(`pmm.cpp:211-217`),整块置 refcount=1(`pmm.cpp:228-231`),映射只碰前 count 页,尾页 refcount 留 stale 1。`free_pages` 只 free head 按 order 回收整块、不查 per-page 计数,所以尾页不会泄漏 buddy(整块回收)。`test_shm_stat` 用 3 页段(实际 alloc 4 页)验证过账不爆,这里提一句不展开。

## 小结

- **shm 的价值不是「比 pipe 快」,是「机制不同」**:pipe 搬数据(字节流过内核 buffer,两次 copy)、shm 搬地址(页表直接共享物理页,零 copy + 无 syscall 可见)。四个 syscall(shmget/shmat/shmdt/shmctl)真注册在 `syscall.cpp:224-227`,号 29/30/31/67 跟 Linux x86_64 ABI 对齐。
- **分层铁律**:`ShmRegistry`(固定 16 槽纯逻辑表,key→segment,只管簿记 + nattach/marked 状态机,零 kernel-only 依赖)vs `sys_shm`(物理页生命周期层,alloc_pages/map/unmap/free_pages)。承 071 的 `FifoRegistry` 模子,跟 081 tmpfs「纯逻辑 vs boot I/O」是同一套切法。
- **mapcount 闭环双计数器真相**:`pte_count`(PTE 映射数,起 0)+ `refcount`(所有权,alloc 给段基线 1)。shmat 同时 inc 两者;teardown 走 `pte_count_dec_and_test` 两级 test(pte_count 先减、归零才 dec refcount)——段页即便所有 attach 退出、pte_count 归零,refcount 仍 > 0 不放,只有 IPC_RMID 显式 free 才回收。源码注释(shm.hpp:18-22、sys_shm.cpp:12-15)把基线说成「pte_count 置 1」是历史措辞漂移,**以 pmm.cpp:229-230 为真相**。
- **shmdt 长度陷阱**:两 SHM 映射会合并成一个 VMA(flags 全等 + 无 backing),shmdt 不能取 VMA 跨度(会拆邻居页),必须用 `translate(addr) → find_by_phys → seg->page_count` 取段自己的页数当权威长度,`remove()` 戳洞处理合并 VMA 切分。`test_shm.cpp:277-319` 专门回归兜底。
- **诚实边界**:ring0 测用栈上 AddressSpace + 空壳 Task 模拟两进程(不是真 libc 跑通);`ShmRegistry` 设计上可链 host 单测但目前没配(对照 fifo 有);shmid_ds 精简五字段(跟 glibc 互操作差一截);IPC_SET/SHM_LOCK 返 ENOSYS;无权限强制、无 sequence number、SMP TOCTOU 文档化、buddy 尾页 refcount 残留是已知非严格性。这些不假装做了,留给后续工程债。
