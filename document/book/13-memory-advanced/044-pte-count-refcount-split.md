---
title: 044 · 物理页的两本账:映射计数与所有权引用
---

# 044 · 物理页的两本账:映射计数与所有权引用

> 一块物理内存页,在内核里有两种完全不同的「被谁占着」要记:**有几个页表项(PTE)映射了它**(映射维度),以及**有谁「拥有」它**(所有权维度)。咱们以前把这两本账混成一本——一个 `mapcount`,既数映射、又决定释放。这一章把它拆成两本:`pte_count`(只数映射)+ `refcount`(决定释放),并用一个类型化的 `PhysRef<Tag>` 把「这页归谁」管起来。
>
> punchline 是个根因故事:混账的时候,页缓存为了「自己的页别在拆映射时被释放」,在 `pte_count` 上偷偷 `+1` 兜底(幻影引用)。这个 `+1` 在单核、顺序好的时候没事,可一旦 GCC 自举跑真编译、`lto_plugin` 跨进程共享缓存页、SMP 并发拆映射,这个兜底就盖不住所有顺序——偶发 double-free 风格的页腐蚀,`ld`/`cc1` 莫名崩。拆成两本账后,「缓存拥有」走 `refcount`(类型化、不会被映射拆减误伤),「映射计数」走 `pte_count`(纯计数),兜底的 `+1` 就不需要了——bug 从类型层没了。
>
> B 档:这是内部重写、无新用户可见能力,lab 靠 `test_pmm_pte_count` + 看不变式验证两本账各自正确、合起来等价于旧的单账语义。

## 这章咱们要点亮什么

1. **一块物理页有两个独立的「占着」维度**:映射维度(几个 PTE 指向它)和所有权维度(谁拥有它、能不能释放)。这两个维度的「到 0」事件是不同的,混成一本账会互相误伤。
2. **释放该由「所有权」决定,不该由「映射」决定**:映射到 0 只说明「没人映射了」,可页可能还被缓存拥有(下次还能命中);只有所有权到 0 才真释放。旧的 `mapcount_dec_and_test` 把这两件事绑在一起,逼得缓存用幻影 `+1` 兜底。
3. **`pte_count_dec_and_test` 内部联动两本账**:拆映射时,`pte_count` 减;`pte_count` 到 0 时,连带减一个所有权 `refcount`;`refcount` 到 0 才真释放。对 7 个拆映射的调用点来说,接口没变(还是 `pte_count_dec_and_test` 返 true=已释放),但语义内部化了。
4. **`PhysRef<Tag>` 是类型化的所有权句柄**:页缓存用 `CachePhysRef`、匿名页用 `AnonPhysRef`、页表用 `PageTablePhysRef`——类型在编译期标记「这页归谁」,RAII 析构时减对应的 `refcount`。幻影 `+1` 被「缓存领一个 `CachePhysRef`」替代,语义清晰、不会忘配对。

## 主线一:为什么一个 `mapcount` 不够

### 旧账:既数映射、又决定释放

咱们物理页管理器(PMM)给每页维持一个 `mapcount`——「几个 PTE 映射了这页」。分配时置 1(分配者拥有一个映射),每次 `mmap`/缺页建映射 `+1`,拆映射 `mapcount_dec_and_test`(减到 0 返 true,调用方 `free_page`)。

这套在「一个页只被一个地址空间映射、用完就拆」时自洽。可页缓存(file-backed page cache)打破了这个假设:**一个缓存页可能同时被多个进程映射**(两个进程 mmap 同一个文件的同一段),而且**即使所有映射都拆了,缓存自己还想留着这页**(下次命中),不该释放。

### 幻影 +1:混账下的无奈兜底

旧账里,「缓存想留着」和「映射到 0 就释放」是冲突的。缓存为了不让自己的页在某个进程拆映射(mapcount 减到 0)时被释放,在把页交给映射之前,**偷偷给 mapcount `+1`**——这个 `+1` 不对应任何真实 PTE,纯粹是「缓存占了一个名额」,让 mapcount 永远不会因为映射拆完就到 0。

这个幻影 `+1` 在单核、顺序良好时能 work。可它有几个治不好的毛病:

- **语义糊**:mapcount 这个数读出来,你不知道其中几个是真实 PTE、几个是幻影。调试时看到一个页 mapcount=3,可能是 3 个映射、或 2 个映射+1 幻影、或……
- **配对容易漏**:幻影 `+1` 要在缓存驱逐时 `-1`,这个配对散在各处,某个少见的驱逐路径漏了就 leak 或 underflow。
- **SMP 下盖不住所有顺序**:`lto_plugin` 跨进程共享缓存页 + 多核并发拆映射,某些交错顺序下幻影兜底算出的「还该活着」和实际不符,导致页被提前释放(别的进程 PTE 指向被释放+重用的页,读到别的数据)——这就是 GCC 自举时 `ld`/`cc1` 偶发崩的根因。

### 根因:两个维度被绑成了一个数

问题的根不是「+1 算错了」,是**两个本该独立的维度被塞进了一个数**:

- 「几个 PTE 映射这页」(映射维度)——纯计数,用于判断「拆了这个 PTE,还有没有别的映射」。
- 「这页归谁所有、能不能释放」(所有权维度)——缓存拥有、匿名页拥有者拥有、页表拥有;到 0 才释放。

这两个维度的「到 0」是不同事件。混成一本账(mapcount),拆映射(mapcount--)和释放判定(mapcount==0)就绑死了,缓存只能用幻影 +1 拆开它们——而幻影是运行时约定,不是类型保证,早晚漏。

## 主线二:拆成 `pte_count` + `refcount` 两本账

### 两本账各自的语义

[pmm.hpp](kernel/mm/pmm.hpp) 给每页维持**两个**独立计数:

```cpp
// 映射维度:几个 PTE 映射这页。纯计数,绝不自己决定释放。
void pte_count_inc(uint64_t phys);
bool pte_count_dec_and_test(uint64_t phys);   // 见下:内部联动 refcount

// 所有权维度:几个「拥有者」(alloc 基线 1 + 缓存/shm owner)。到 0 才真释放。
void refcount_inc(uint64_t phys);
bool refcount_dec_and_test(uint64_t phys);     // true => refcount 到 0,页已释放
```

**释放只由 `refcount` 决定**:`refcount_dec_and_test` 到 0 才把页还给 buddy 分配器。`pte_count` 到 0 不释放,只是「没有映射了」。

**`pte_count_dec_and_test` 内部联动**:拆映射时调用方调的是 `pte_count_dec_and_test`(接口跟旧的 `mapcount_dec_and_test` 一样,7 个拆映射调用点不用改)。它内部:

```cpp
bool PMM::pte_count_dec_and_test(uint64_t phys) {
    if (__atomic_sub_fetch(&pte_count_storage_[...], 1, ACQ_REL) != 0)
        return false;                 // 还有别的 PTE 映射,啥都不动
    return refcount_dec_and_test(phys);  // 最后一个映射没了 → drop 一个所有权 ref(可能释放)
}
```

也就是:`pte_count` 到 0 时,**连带减一个所有权 `refcount`**(因为「地址空间拥有这页」这个所有权跟着最后一个映射没了)。`refcount` 到 0 才真释放。调用方看到的契约不变(返 true = 页已释放),但内部把「映射到 0 → 释放」拆成了「映射到 0 → 减所有权 → 所有权到 0 → 释放」。

> 这个联动是给「地址空间拥有」那一份 ref 配对的:`alloc_page` 给 `refcount=1`(地址空间拥有的基线),每次建映射 `pte_count_inc`(只数映射、不动 refcount),拆映射 `pte_count_dec_and_test`(pte_count 到 0 时减那个基线 refcount)。所以一个纯匿名页:alloc(refcount=1,pte_count=0)→ 映射(pte_count=1)→ 拆映射(pte_count=0 → 减 refcount 到 0 → 释放)。链路闭合。

### 缓存页:用 `refcount` 而不是幻影 +1

页缓存拥有一个页时,用 `refcount`(不是 `pte_count`)登记所有权:

```cpp
// 缓存命中/装入时,缓存领一个所有权 ref(CachePhysRef RAII 析构减它)
refcount_inc(phys);   // 缓存拥有
// 映射这个缓存页给别人时,建映射照常 pte_count_inc(只数映射)
```

这样即使所有映射都拆了(`pte_count` 到 0,联动减了地址空间的 refcount),缓存的那个 `refcount` 还在(>0),页不会被释放——下次命中还能用。缓存驱逐时,`CachePhysRef` 析构 `refcount_dec_and_test`,到 0 才真释放。

**幻影 +1 不需要了**:缓存拥有走的是 `refcount`(类型化、RAII 管理、不会忘配对),不再靠在 `pte_count` 上偷 `+1` 兜底。旧的 `lto_plugin` 腐蚀路径(幻影算错 → 页被提前释放)从类型层消失。

## 主线三:`PhysRef<Tag>`——类型化的所有权句柄

### 为什么要类型化

「这页归谁所有」可以有好几种:页缓存拥有(`CachePhysRef`)、匿名页拥有(`AnonPhysRef`)、页表拥有(`PageTablePhysRef`)。如果都用裸 `refcount_inc`/`dec`,「这是谁的 ref」只靠注释和记忆,容易把一个 `CachePhysRef` 的 ref 当成匿名页的 ref 减错。

`PhysRef<Tag>` 用模板参数 `Tag` 在**编译期**标记「这页归谁」:[phys_ref.hpp](kernel/mm/phys_ref.hpp)

```cpp
// 每种拥有者一个 tag 类型;PhysRef<Tag> 析构时减 refcount
struct CachePhysRef { /* 析构 refcount_dec_and_test */ };
struct AnonPhysRef  { /* ... */ };
struct PageTablePhysRef { /* ... */ };
```

页缓存的 `CachedPage` 结构里,「这页归我」存成 `CachePhysRef own;`(而不是裸 phys)。构造时 `CachePhysRef::alloc` 领一个 refcount,析构时 RAII 减掉。类型保证了「缓存的 ref 只用 CachePhysRef 管」,不会和别的拥有者搞混。

### RAII:析构即减 ref

`PhysRef` 是 RAII 句柄:构造 = 领 ref(inc refcount),析构 = 还 ref(dec_and_test)。拥有者(`CachedPage`、匿名页描述符)把它当成员,自己析构时 `own` 成员自动析构 → 自动减 refcount → 自动可能释放。不用手动记「这里要 dec」——作用域到了就减。

这取代了旧的「散在各处的 manual refcount_inc/dec + 幻影 +1」:所有权的增减绑在对象生命周期上,不会漏配对。漏配对(某个驱逐路径忘 dec)是旧账 leak/double-free 的常见源,RAII 从结构上消除它。

## 三件事合起来看

- **拆两本账**(主线二):`pte_count` 数映射、`refcount` 决定释放,各自独立、`pte_count_dec_and_test` 内部联动。
- **类型化所有权**(主线三):`PhysRef<Tag>` + RAII,替换裸 refcount 操作和幻影 +1。
- **幻影 +1 退休**:缓存拥有走 `refcount`(类型化),不再在 `pte_count` 上偷名额。

合起来,旧账那套「`mapcount` 既数映射又决定释放 + 缓存幻影 +1 兜底」被「两本独立账 + 类型化所有权」替代。`lto_plugin` 那种「幻影算错 → 页提前释放 → 别的进程读到垃圾」的腐蚀路径,从类型结构上没了——不是「+1 算对了」,是「不需要 +1 了」。

> 这一章和隔壁 [078 · VFS 地基](../08-filesystem/078-vfs-groundwork-lookup-refcount.md)(inode 引用计数)是姊妹:那章讲 **inode 层**的引用计数(几个 fd 指着 inode),这章讲**物理页层**的引用计数(几个映射/拥有者占着页)。两层都是「对象生命周期 + 引用计数」,但对象不同、计数语义不同。VFS 那层解决「inode 指针有效性」,这层解决「物理页释放正确性」。

## 范围与边界(诚实说)

- **`pte_count_dec_and_test` 契约不变是刻意为之**:为了让 7 个拆映射调用点(`free_subtree`/`clear_user_mappings`/`sys_munmap`/CoW-old 等)不用改,新接口保留了「返 true = 页已释放」的旧契约,只是内部把「释放」拆成了「减 refcount → 到 0 才释放」。所以这 7 个调用点不需要同时 `free_page`(dec_and_test 已经内部释放了)——这是个 footgun,踩过的迁移有专门注释提醒。
- **deferred-free 变体(`_no_free`)**:某些路径(CoW 跨核 TLB shootdown)需要「先判定可释放、延迟到 shootdown 完再真释放」,所以有 `pte_count_dec_and_test_no_free`/`refcount_dec_and_test_no_free`,返 true 但不释放、调用方稍后 `free_page`。这是隔壁并发修复弧加的,这章的拆账本身不依赖它。
- **SysV 共享内存(shm)这章没接**:shm 也是物理页的一个拥有者(`shm attach` 给 refcount +1),但咱们这轮没回迁 shm,所以 shm 的 refcount 操作从相关 commit 里剥掉了。接 shm 是后续(它会自然走 `refcount_inc` 加一份 owner ref)。
- **本机 WSL2 验不了 SMP 真触发**:这套拆账在单核正确(测试覆盖),SMP 下的真并发腐蚀要 nested-KVM 透传 + 真双核压力才暴露。`lto_plugin` 那个根因故事是 CinuxOS 在真机上调试出来的,咱们本机只能验「两本账各自正确 + 合起来等价旧语义」。

> 验证靠 `test_pmm_pte_count`(从 `test_pmm_mapcount` 改名而来):pte_count/refcount 各自的增减、`pte_count_dec_and_test` 的联动释放、缓存页 refcount 保活。`run-kernel-test` 跑过、绿。
