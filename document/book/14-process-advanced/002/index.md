---
title: 002 · clone / futex / TLS
---

# 002 · clone / futex / TLS

> fork 造的是进程,很多时候你要的是线程——共享同一块地址空间、同一套文件描述符,只是各自有独立的执行流和栈。这一章实现 clone,配上 futex(线程怎么同步)、TLS(每线程的局部存储)、cleartid(pthread_join 的底),把 POSIX 线程的内核基础铺好。

## 本章路线

- [01 · futex:线程怎么在共享内存上高效同步](01-futex.md)
- [02 · 从复制进程到按需共享:clone、futex 与线程](02-clone.md)
