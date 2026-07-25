---
title: 006 · 让 ext2 能写:从只读到可建可删
---

# 006 · 让 ext2 能写:从只读到可建可删

> 补上 ext2 的「写」这一半:能写、能建、能删,改动要真正落回 AHCI 磁盘。用户态敲 echo hi > /hello.txt,重启再读,hi 还在——这才是「活的」文件系统。

## 本章路线

- [01 · 导引:点亮什么与为什么](01-intro.md)
- [02 · 设计图:写的统一姿势与分配器](02-design.md)
- [03 · 写回:read-modify-write 与唯一的 DMA 缓冲](03-writeback.md)
- [04 · 两个分配器:扫位图找空闲](04-allocator.md)
- [05 · 写文件:直接块、单间接、截断的循环](05-write-file.md)
- [06 · 建文件与建目录:create / mkdir 的资源管理与回滚](06-create.md)
- [07 · 删除与目录项增删:unlink 释放、split rec_len](07-unlink.md)
- [08 · 接上用户态:sys_creat 与 sys_rmdir](08-userspace.md)
- [09 · 调试现场:sys_creat 首次深入 ext2 触发 GPF](09-debug.md)
- [10 · 收尾:验证与下一站](10-wrapup.md)
