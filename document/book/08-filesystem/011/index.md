---
title: 011 · ProcFS:把进程列表挂进 /proc
---

# 011 · ProcFS:把进程列表挂进 /proc

> 沿用 DevFS 的「内存型虚拟 FS」范式做进程自省:/proc 根目录枚举活进程的 pid,/proc/<pid>/ 下挂 stat、cmdline 这些伪文件——读它们时现从内核进程表里取数、拼成文本返回。

## 本章路线

- [01 · 导引:范式与点亮什么](01-intro.md)
- [02 · 动态根目录:定长 inode 池与 readdir 枚举活进程](02-readdir.md)
- [03 · 伪文件与 procfs_pseudo:读时现场生成文本](03-pseudo-file.md)
- [04 · 收尾:TOCTOU 边界、文件门、验证与小结](04-wrapup.md)
