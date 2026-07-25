---
title: 14 · 进程与线程增强
---

# 14 · 进程与线程增强

> F3 进程弧 + F8 多路复用弧:在 000-035 卷 06-process 的 context_switch / 调度器 / 同步原语之上,补 POSIX 信号、clone/futex/TLS 线程、进程组/会话、调度类、可插拔调度策略;再延伸到 pipe 增强与命名 FIFO、SysV 共享内存、poll/select 多路复用、timer_queue 与 stats_kthread 两件内核基础设施。读法:先 000-035 卷 06-process(基础),再本卷。

## 阅读顺序

- [001 · POSIX 信号](001/)
- [002 · clone / futex / TLS](002/)
- [003 · 进程组与 waitpid 阻塞](003/)
- [004 · 调度类与 SIGSTOP/CONT](004/)
- [005 · Pipe 增强与命名 FIFO](005/)
- [006 · SysV 共享内存](006/)
- [007 · poll / select](007/)
- [008 · timer_queue 与 stats_kthread](008/)
