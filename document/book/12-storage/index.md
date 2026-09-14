---
title: 12 · 存储驱动
---

# 12 · 存储驱动

> 块设备 / 存储驱动相关:`IBlockDevice` 契约下的具体实现——AHCI DMA(`12-storage/001`)、NVMe(`12-storage/002`)、VirtIO-blk/net(`12-storage/002`)等。它们都复用 11-foundation 立的 `IBlockDevice` + `DmaPool` + `PrdtBuilder`,各转各的硬件格式。读法:先 11-foundation 卷的 `11-foundation/003`(`IBlockDevice` 抽象)再本卷。

## 阅读顺序

- [001 · AHCI DMA 迁移](001/)
- [002 · NVMe 与 VirtIO:两种 PCI 设备抽象并存](002/)
