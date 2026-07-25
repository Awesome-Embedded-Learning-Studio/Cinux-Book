---
title: 009 · busybox 当 PID1:init 不是 fork 出来的
---

# 009 · busybox 当 PID1:init 不是 fork 出来的

> 让 busybox 的 `init` applet 当 PID 1,按 `/etc/inittab` respawn `/bin/sh`——顺带把"PID 1 为什么永远是 1"讲透:init 线程入口领号、execve 保 pid。

## 本章路线

- [01 · busybox 当 PID1:init 不是 fork 出来的](01-intro.md)
- [02 · 代码路线:PID 1、execve 保 pid、rt_sigtimedwait、/dev/console 与 EFAULT](02-implementation.md)
- [03 · 收尾:非 GUI 生产启动验收与诚实边界](03-wrapup.md)
