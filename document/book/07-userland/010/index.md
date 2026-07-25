---
title: 010 · Linux ABI 拼图:让 glibc/musl/busybox 跑起来
---

# 010 · Linux ABI 拼图:让 glibc/musl/busybox 跑起来

> 让 glibc 和更激进的 musl 程序在启动各阶段不被一个缺掉的 ABI 卡住——内核要么真给数据、要么诚实地返 `-ENOSYS` 让 libc 优雅降级。十五个号(十四个 Linux ABI + 一个 Cinux 专有 `cinux_exit`),九个真实现、五个 stub,讲透「stub 返 ENOSYS 才是对的」这件反直觉事。

## 本章路线

- [01 · Linux ABI 拼图:让 glibc/musl/busybox 跑起来](01-intro.md)
- [02 · 真实现:用户态启动各阶段真要用数据的几个](02-real-impl.md)
- [03 · stub 的艺术:`-ENOSYS` 是正面信号,不是没做完](03-stub-art.md)
- [04 · stub 文件里的真实现:`tkill` / `setitimer` / `sched_getaffinity`](04-stub-real-impl.md)
- [05 · `cinux_exit`:暴露给 ring3 的 QEMU 退出口(不是 `sys_exit`)](05-cinux-exit.md)
- [06 · 收尾:验证、诚实边界、没做的与小结](06-wrapup.md)
