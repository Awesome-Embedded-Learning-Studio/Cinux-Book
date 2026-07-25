---
title: 03 · mapcount 闭环:段自带 refcount 基线 1 防 teardown 误放
---

# mapcount 闭环:段自带 refcount 基线 1 防 teardown 误放

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

## 源码注释的措辞出入,这里得讲清

真正漂移的只有两处头注释:`shm.hpp:18-22` 和 `sys_shm.cpp:12-15`,它们把 alloc 基线说成「`pte_count` 置 1」:

```cpp
 * pte_count model (mirrors fork CoW, kernel/proc/fork.cpp): alloc_pages sets
 * each page to 1 (the segment's own reference); shmat adds +1 per page; ...
```

([sys_shm.cpp](../../../kernel/syscall/sys_shm.cpp#L12-L15),有删节。)这跟 `pmm.cpp:229-230` 的真值(`refcount=1 / pte_count=0`)对不上。措辞是历史遗留——旧实现是单 mapcount,注释写「置 1」是对的;batch 3 拆成双计数器后,基线挪到了 refcount 上,这两处头注释没同步改。**讲教程以 `pmm.cpp` 为真相**:`refcount=1` 是基线、`pte_count=0` 起步、shmat 同时 inc 两者。运行时正确性靠 `pte_count_dec_and_test` 的两级 test 守住(段页存活),注释措辞模糊但不影响行为。这种「注释跟代码漂移」是工程里常见的债,记一笔省得你拿着头注释去 grep `pte_count = 1` 找到对不上。

> grep 时认准这两处**头注释**(`shm.hpp:18-22` 和 `sys_shm.cpp:12-15`),别误伤同文件 `sys_shm.cpp:182-185` 的**安装注释**——那段写的是「map-ownership ref (refcount); the segment's own ref (alloc baseline=1)」,明确把基线归到 refcount,跟 `pmm.cpp` 真值一致,是 batch 3 重写过的正确版本。漂移只在头注释那一处,别拿对的注释去印证错的注释。
