---
title: 001 · 给地址空间一本账:VMA 区域记账与 mmap
---

# 001 · 给地址空间一本账:VMA 区域记账与 mmap

> 给 `AddressSpace` 配一本区域账本——VMA(Virtual Memory Area),让 mmap、堆扩展、共享内存、按需读文件都来问它、改它,再在账本上实现 `mmap`。

## 本章路线

- [01 · 按需分页:为什么要等访问到了才给内存](01-demand-paging.md)
- [02 · 给地址空间一本账:VMA 区域记账与 mmap](02-vma-mmap.md)
