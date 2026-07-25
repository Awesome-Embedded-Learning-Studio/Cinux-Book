---
title: 002 · NVMe 与 VirtIO:两种 PCI 设备抽象并存
---

# 002 · NVMe 与 VirtIO:两种 PCI 设备抽象并存

> AHCI 只是 PCI 设备的一种脾气。这一章一口气加两个风格完全不同的 PCI 设备驱动(NVMe 队列+doorbell、VirtIO 特性谈判+共享环),让它们在同一个内核里并存,最后都接回 `IBlockDevice`/`NetDevice`。

## 本章路线

- [01 · 导引:点亮什么、为什么](01-intro.md)
- [02 · 两条主线:NVMe 队列+doorbell 与 VirtIO 特性谈判+virtqueue](02-implementation.md)
- [03 · 落点与诚实的边界](03-wrapup.md)
