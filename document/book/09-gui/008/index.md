---
title: 008 · GUI 解耦:host-neutral core、Host 表、userspace host
---

# 008 · GUI 解耦:host-neutral core、Host 表、userspace host

> 把 GUI 从内核拔成 host-neutral core,内核只剩填 Host 表的薄接缝。

## 本章路线

- [01 · 问题与边界:被焊死的桌面 + host-neutral core](01-boundary.md)
- [02 · Host ABI 表 + GuiCore pump + Region](02-host-abi.md)
- [03 · userspace host + 内核薄接缝 + 数据路径](03-userspace-host.md)
- [04 · 验证 + 没做的 + 小结](04-wrapup.md)
