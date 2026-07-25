---
title: 003 · 在页上切块:内核堆分配器
---

# 003 · 在页上切块:内核堆分配器

> PMM 和 VMM 的最小单位都是一整页 4KB,想存个 64 字节的结构体都得拿一整页。这一章给内核装一个堆分配器:它在一串 VMM 映射好的页上做细粒度的切块、回收、合并,用 first-fit + 分裂 + 合并管理一块空闲链,空闲链用光了还会自动向 VMM 要更多页。再把全局 `operator new` / `delete` 接管到这套堆上——从此内核里的 C++ 代码写 `new` / `delete`,落到的就是自己的堆。

## 本章路线

- [01 · 点亮什么、为什么、设计图](01-intro.md)
- [02 · 代码路线:BlockHeader / init / alloc / 对齐](02-implementation.md)
- [03 · 代码路线续:free / expand / crt_stub](03-free-expand.md)
- [04 · 调试现场与收尾](04-debug.md)
