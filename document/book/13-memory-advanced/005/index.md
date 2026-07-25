---
title: 005 · 物理页的两本账:映射计数与所有权引用
---

# 005 · 物理页的两本账:映射计数与所有权引用

> 把混账的 `mapcount` 拆成 `pte_count`(映射维度)+ `refcount`(所有权维度),用类型化的 `PhysRef<Tag>` 管所有权,消除缓存页靠幻影 `+1` 兜底的腐蚀 bug。

## 本章路线

- [01 · 导引:一块物理页的两本账](01-intro.md)
- [02 · 拆账:`pte_count` 与 `refcount` 各司其职](02-split-accounts.md)
- [03 · 类型化所有权:`PhysRef<Tag>` 与收尾](03-physref.md)
