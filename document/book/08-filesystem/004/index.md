---
title: 004 · 让用户态能 open/read 文件:VFS 系统调用与 shell
---

# 004 · 让用户态能 open/read 文件:VFS 系统调用与 shell

> 给 VFS 装门:把 open/read/write/close/getdents 接到 VFS 管线,libc 包一层薄壳,让 shell 长出 cat 和 ls——VFS 真正被人用上了。

## 本章路线

- [01 · 导引:点亮什么与为什么](01-intro.md)
- [02 · 设计图:系统调用怎么落到 VFS](02-design.md)
- [03 · 实现:sys_open/read/write/getdents、libc、shell](03-implementation.md)
- [04 · 调试现场与验证](04-debug.md)
- [05 · 收尾:下一站与参考](05-wrapup.md)
