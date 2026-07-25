---
title: 07 · 用户态
---

# 07 · 用户态

> 从第一次跳进 Ring 3、立特权隔离的墙,到 SYSCALL/SYSRET 服务通道、常驻 shell;再对齐 Linux ABI、铺 auxv 跑起真 musl/busybox,补 TTY 行规范、PTY 多路终端、ELF 动态链接、socket API 与 PID1 init——这一卷把内核从「能跑」推到「真用户态生态能跑」。

## 阅读顺序

- [001 · 第一次跳进 Ring 3:用户态与特权隔离](001/)
- [002 · 让用户态会说话:SYSCALL/SYSRET 系统调用](002/)
- [003 · 给内核一个能对话的用户态:shell](003/)
- [004 · musl 静态移植:对齐 Linux ABI、铺初始栈](004/)
- [005 · TTY 行规范:让 shell 真正能交互](005/)
- [006 · ELF 动态链接:内核只装 interp](006/)
- [007 · PTY:伪终端把 console 单例变成多路终端](007/)
- [008 · busybox 跑起来 + socket API](008/)
- [009 · busybox 当 PID1:init 不是 fork 出来的](009/)
- [010 · Linux ABI 拼图:让 glibc/musl/busybox 跑起来](010/)
