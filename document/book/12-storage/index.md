---
title: 存储驱动（v1.0.0 弧）
---

# 存储驱动（v1.0.0 回迁弧）

> 块设备 / 存储驱动相关:`IBlockDevice` 契约下的具体实现——AHCI DMA(039)、NVMe(后续)、VirtIO-blk(后续)等。它们都复用 11-foundation 立的 `IBlockDevice` + `DmaPool` + `PrdtBuilder`,各转各的硬件格式。读法:先 038(IBlockDevice 抽象,在 11-foundation 卷)再本卷。
