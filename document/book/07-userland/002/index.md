---
title: 002 · 让用户态会说话:SYSCALL/SYSRET 系统调用
---

# 002 · 让用户态会说话:SYSCALL/SYSRET 系统调用

> 用户程序执行 `syscall` 指令,硬件瞬间把我们送到 Ring 0 的 `syscall_entry`;内核干完活,再用 `sysretq` 把它原样送回 Ring 3。一行真正由 Ring 3 代码打印的 `[USER] Hello from Ring 3!`,就是这条受控服务通道成立的证据。

## 本章路线

- [01 · 让用户态会说话:SYSCALL/SYSRET 系统调用](01-intro.md)
- [02 · 代码路线:MSR、syscall_entry、dispatch 表与用户态编译](02-implementation.md)
- [03 · 调试现场:movaps #GP、测试全跳过与 sys_exit halt](03-debug.md)
- [04 · 收尾:验证、下一站与参考](04-wrapup.md)
