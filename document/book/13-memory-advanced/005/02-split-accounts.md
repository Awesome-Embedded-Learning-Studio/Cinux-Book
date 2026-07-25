---
title: 02 · 拆账:`pte_count` 与 `refcount` 各司其职
---

# 拆账:`pte_count` 与 `refcount` 各司其职

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

[pmm.hpp](../../../kernel/mm/pmm.hpp) 给每页维持**两个**独立计数:

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
