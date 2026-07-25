---
title: 003 · 给文件一个统一接口:VFS 内核层
---

# 003 · 给文件一个统一接口:VFS 内核层

> 在「认得文件」之上搭一层虚拟文件系统:统一的文件对象(inode)、统一的操作接口、一张「路径 → 文件系统」的挂载表,让 ramdisk 实现这套接口挂上来。

## 本章路线

- [01 · 导引:点亮什么与为什么](01-intro.md)
- [02 · 设计图:VFS 四件套](02-design.md)
- [03 · 实现:inode、FileSystem、挂载表、FDTable、ramdisk](03-implementation.md)
- [04 · 调试现场与验证](04-debug.md)
- [05 · 收尾:下一站与参考](05-wrapup.md)
