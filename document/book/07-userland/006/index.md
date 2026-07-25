---
title: 006 · ELF 动态链接:内核只装 interp,重定位交给 ldso
---

# 006 · ELF 动态链接:内核只装 interp,重定位交给 ldso

> 内核在动态链接里只做三件小事:认出 `PT_INTERP`、把 ldso 装载进地址空间、喂对 auxv。剩下的重定位全交给 musl 的 ldso——对齐 Linux,内核不自建 loader。

## 本章路线

- [01 · ELF 动态链接:内核只装 interp,重定位交给 ldso](01-elf-dynamic.md)
