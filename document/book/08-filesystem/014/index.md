---
title: 014 · VFS 地基重做:组件遍历、引用计数、inode 缓存
---

# 014 · VFS 地基重做:组件遍历、引用计数、inode 缓存

> 把 VFS 三块「凑合」换成正经该长那样的形态:挂载点感知的组件遍历、inode 引用计数、ext2 inode 缓存——为后面的 dentry cache、真用户态、SMP 并发打底。

## 本章路线

- [01 · 导引:三块凑合要换掉](01-intro.md)
- [02 · 主线一:从字符串规范化到 vfs_lookup 组件遍历](02-lookup.md)
- [03 · 主线二:inode 引用计数与 last-close release](03-refcount.md)
- [04 · 主线三:ext2 inode 缓存——堆分配 + 引用计数驱动回收](04-inode-cache.md)
- [05 · 收尾:三块地基拧成一根绳 + 范围与边界](05-wrapup.md)
