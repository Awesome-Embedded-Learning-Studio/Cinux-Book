---
title: 001 · fork / execve / waitpid:让进程能生、能换、能收尸
---

# 001 · fork / execve / waitpid:让进程能生、能换、能收尸

> 把 Unix 进程模型的三大原语——`fork`(生)、`execve`(换)、`waitpid`(收)——一次性接进内核,搭起 PID 分配器、TCB 家谱字段、Copy-On-Write 页表和 ELF 加载器。

## 本章路线

- [01 · 导引:点亮什么、为什么、设计图](01-intro.md)
- [02 · 代码路线:PID/fork/CoW/execve/waitpid/syscall](02-implementation.md)
- [03 · 调试现场:fork 还没让子进程「返回 0」](03-debug.md)
- [04 · 收尾:验证 + 没做的 + 下一站 + 参考](04-wrapup.md)
