---
title: 006 · SysV 共享内存
---

# 006 · SysV 共享内存

> IPC 四件套走到这一章,shm 这一刀更狠:它根本不搬字节,它搬地址。两个进程各自 shmat 一块登记好的物理页到自己地址空间,底下落到的是同一批物理帧——写者写一个字节、读者无需任何 syscall 就看到新值。可这套机制藏着一颗定时炸弹:进程退出时地址空间析构绝不能把「段还在用」的页误放回 free pool。

## 本章路线

- [01 · 导引:不是搬字节,是搬地址](01-intro.md)
- [02 · ShmRegistry 表层:承 071 的模子](02-registry.md)
- [03 · mapcount 闭环:段自带 refcount 基线 1](03-mapcount.md)
- [04 · shmdt 长度陷阱:取段 page_count](04-shmdt.md)
- [05 · 验证、没做的、小结](05-wrapup.md)
