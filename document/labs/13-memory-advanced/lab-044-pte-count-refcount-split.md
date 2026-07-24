---
title: Lab 044 · 物理页两本账验证:pte_count 与 refcount
---

# Lab 044 · 物理页两本账验证:pte_count 与 refcount

> 对应 `document/book/13-memory-advanced/044-pte-count-refcount-split.md`。验证档 **B 档**:这章是内部重写(把单一 `mapcount` 拆成 `pte_count` + `refcount` 两本账 + `PhysRef<Tag>` 类型化所有权),无新用户可见能力。lab 靠跑 `test_pmm_pte_count` + 查不变式验证「两本账各自正确、合起来等价旧语义、缓存页靠 refcount 保活」。

## 目标

确认四件事:

1. **`mapcount` 全仓零残留**:改名彻底(包括注释),`pte_count_*`/`refcount_*` 取代;
2. **`pte_count_dec_and_test` 联动正确**:pte_count 到 0 时连带减一个 refcount,refcount 到 0 才释放(返 true);不到 0 返 false、不释放;
3. **缓存页靠 refcount 保活**:一个页即使所有映射拆完(pte_count 到 0),只要缓存还持 `CachePhysRef`(refcount > 0),就不释放;
4. **调用点无双 free**:7 个拆映射调用点不再 `if (dec_and_test) free_page`(dec_and_test 内部释放了),没有残留的旧两步模式。

## 步骤

### 1. 改名彻底:`mapcount_` 零残留

```bash
grep -rn 'mapcount_' kernel/ | head
grep -c 'pte_count_\|refcount_' kernel/mm/pmm.hpp
```

第一行应**无输出**(或只剩无害的文档/历史注释,不含 `mapcount_` 标识符)。第二行应有若干 `pte_count_*`/`refcount_*` 声明。这验主线一/二的「拆账 + 改名」彻底。

### 2. 跑 test_pmm_pte_count(改名后的测试)

```bash
cmake --build build -j$(nproc) 2>&1 | grep -iE 'pmm|pte_count|Built target' | head
cmake --build build --target run-kernel-test 2>&1 | grep -aE 'pmm_pte_count|Tests:' | head
```

应看到 `test_pmm_pte_count.cpp`(从 `test_pmm_mapcount.cpp` 改名)编了、机制测 `[PASS]`,末尾 `Tests: 1084 passed, 0 failed`。

### 3. 验联动释放的契约

`test_pmm_pte_count` 覆盖:

- **多次映射、逐个拆**:alloc(refcount=1)→ pte_count_inc 几次 → pte_count_dec_and_test,前几次返 false(还有映射、不释放),最后一次 pte_count 到 0 → 联动减 refcount 到 0 → 返 true(释放);
- **缓存页不释放**:同上,但中间 `refcount_inc`(模拟缓存领 owner)→ 最后一次 pte_count 到 0、refcount 减到 1(缓存还在)→ 返 false、不释放 → 再 `refcount_dec_and_test` 到 0 才释放。

要看清这两条,临时在 [pmm.cpp](kernel/mm/pmm.cpp) 的 `pte_count_dec_and_test` 加 `kprintf("pte_count=%d refcount=%d freed=%d\n", ...)`,重跑能看到「pte_count 到 0 时减 refcount、refcount 决定 freed」。验完删打印。

### 4.(代码审计)调用点无双 free 残留

主线二的 footgun:7 个拆映射调用点不该再有旧的 `if (pte_count_dec_and_test) free_page`(dec_and_test 内部已释放,再 free 是 double-free)。审计:

```bash
grep -rn -A1 'pte_count_dec_and_test' kernel/ | grep -v 'pmm.cpp\|pmm.hpp\|test_pmm' | grep -v '\-\-' | head
```

每个调用点应是**裸调用** `pte_count_dec_and_test(...);`(不接 `if`、不接 `free_page`)。唯一例外是 `_no_free` 变体(deferred CoW 路径,返 true 后延迟 free_page)——那条路径用 `pte_count_dec_and_test_no_free`,和普通版分开。审计应看到「普通版都是裸调用、无 double-free」。

## 范围与边界

- 两本账是**单核正确**的;SMP 下的并发计数正确性靠 `__atomic_acq_rel`(本 lab 不验真双核压力,WSL2 验不了 SMP 真触发)。
- `_no_free` 变体(deferred CoW + IPI TLB shootdown)在隔壁并发修复弧,本 lab 只确认它存在、签名对,不验 shootdown 往返。
- SysV shm 的 refcount owner 这轮没回迁,本 lab 不验 shm attach 的 refcount 增量。
