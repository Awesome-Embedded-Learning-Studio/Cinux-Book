---
title: 03 · 类型化所有权:`PhysRef<Tag>` 与收尾
---

# 类型化所有权:`PhysRef<Tag>` 与收尾

## 主线三:`PhysRef<Tag>`——类型化的所有权句柄

### 为什么要类型化

「这页归谁所有」可以有好几种:页缓存拥有(`CachePhysRef`)、匿名页拥有(`AnonPhysRef`)、页表拥有(`PageTablePhysRef`)。如果都用裸 `refcount_inc`/`dec`,「这是谁的 ref」只靠注释和记忆,容易把一个 `CachePhysRef` 的 ref 当成匿名页的 ref 减错。

`PhysRef<Tag>` 用模板参数 `Tag` 在**编译期**标记「这页归谁」:[phys_ref.hpp](../../../kernel/mm/phys_ref.hpp)

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

合起来,旧账那套「`mapcount` 既数映射又决定释放 + 缓存幻影 +1 兜底」被「两本独立账 + 类型化所有权」替代。「幻影算错 → 页提前释放 → 别的进程读到垃圾」这条腐蚀路径,从类型结构上没了——不是「+1 算对了」,是「不需要 +1 了」。

> 这一章和隔壁 [014 · VFS 地基](../08-filesystem/014/)(inode 引用计数)是姊妹:那章讲 **inode 层**的引用计数(几个 fd 指着 inode),这章讲**物理页层**的引用计数(几个映射/拥有者占着页)。两层都是「对象生命周期 + 引用计数」,但对象不同、计数语义不同。VFS 那层解决「inode 指针有效性」,这层解决「物理页释放正确性」。

## 范围与边界(诚实说)

- **`pte_count_dec_and_test` 契约不变是刻意为之**:为了让 7 个拆映射调用点(`free_subtree`/`clear_user_mappings`/`sys_munmap`/CoW-old 等)不用改,新接口保留了「返 true = 页已释放」的旧契约,只是内部把「释放」拆成了「减 refcount → 到 0 才释放」。所以这 7 个调用点不需要同时 `free_page`(dec_and_test 已经内部释放了)——这是个 footgun,源码里有专门注释提醒。
- **deferred-free 变体(`_no_free`)**:某些路径(CoW 跨核 TLB shootdown)需要「先判定可释放、延迟到 shootdown 完再真释放」,所以有 `pte_count_dec_and_test_no_free`/`refcount_dec_and_test_no_free`,返 true 但不释放、调用方稍后 `free_page`。这是隔壁的并发修复加的,这章的拆账本身不依赖它。
- **SysV 共享内存(shm)这章没接**:shm 也是物理页的一个拥有者(`shm attach` 给 refcount +1),但咱们这章没接 shm,所以 shm 相关的 refcount 操作暂时省掉了。接 shm 是后续(它会自然走 `refcount_inc` 加一份 owner ref)。
- **本机 WSL2 验不了 SMP 真触发**:这套拆账在单核正确(测试覆盖),SMP 下的真并发腐蚀要 nested-KVM 透传 + 真双核压力才暴露。那条根因故事是真机上调试出来的,咱们本机只能验「两本账各自正确 + 合起来等价旧语义」。

> 验证靠 `test_pmm_pte_count`(从 `test_pmm_mapcount` 改名而来):pte_count/refcount 各自的增减、`pte_count_dec_and_test` 的联动释放、缓存页 refcount 保活。`run-kernel-test` 跑过、绿。
