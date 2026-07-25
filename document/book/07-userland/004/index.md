---
title: 004 · musl 静态移植:对齐 Linux ABI、铺初始栈,以及一个被 SMAP 拦下的潜伏 bug
---

# 004 · musl 静态移植:对齐 Linux ABI、铺初始栈,以及一个被 SMAP 拦下的潜伏 bug

> 让内核能跑用真 musl libc 编译的静态程序:对齐 Linux ABI、铺满 auxv 初始栈,然后跑 musl hello——挖出一个被 SMAP 拦下、在 WSL2 上潜伏已久的真 bug。

## 本章路线

- [01 · musl 静态移植:对齐 Linux ABI、铺初始栈,以及一个被 SMAP 拦下的潜伏 bug](01-musl-static.md)
