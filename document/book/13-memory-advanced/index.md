---
title: 13 · 内存管理增强
---

# 13 · 内存管理增强

> F2 内存弧:在 05-memory 卷的 PMM/VMM/堆/地址空间之上,补 VMA、mmap、brk、Page Cache、demand paging、Buddy、Slab,再回迁 v1.0.0 的物理页两本账(pte_count + refcount)。这是 CinuxOS ROADMAP 点名的最大结构瓶颈,阻塞 mmap/CoW/共享内存/文件映射。读法:先 05-memory 卷(基础),再本卷(增强)。

## 阅读顺序

- [001 · 给地址空间一本账:VMA 区域记账与 mmap](001/)
- [002 · 连续的堆,和文件映射背后的真内容:brk 与 Page Cache](002/)
- [003 · 野指针终于会被杀,read() 也走缓存了](003/)
- [004 · 物理分配器升伙伴,小对象交给 Slab,Heap 退役](004/)
- [005 · 物理页的两本账:映射计数与所有权引用](005/)
