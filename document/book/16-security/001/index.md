---
title: 001 · NX / SMEP / SMAP:用三个 CPU 位把内核和用户态隔开
---

# 001 · NX / SMEP / SMAP:用三个 CPU 位把内核和用户态隔开

> 把 x86_64 的三个硬件保护位真正拨开——NX 落实 W^X、SMEP 拦内核执行用户页、SMAP 拦内核读写用户页,把「内核碰用户页」这条路堵死。

## 本章路线

- [01 · 导引:点亮什么、隔离面怎么铺](01-intro.md)
- [02 · 三个位怎么开:NX / SMEP / SMAP](02-implementation.md)
- [03 · 验证与诚实的边界](03-wrapup.md)
