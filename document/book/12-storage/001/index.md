---
title: 001 · AHCI DMA 迁移
---

# 001 · AHCI DMA 迁移

> 037 造好了 DMA 池,038 把 AHCI 包成了 `IBlockDevice` 适配器,可底下驱动还在用散装 DMA。这一章把 `ahci.cpp` 迁到池/构建器上,顺带补 IDENTIFY/FLUSH,让 `AHCIBlockDevice` 的 `block_count()`/`flush()` 从占位变成真东西。

## 本章路线

- [01 · AHCI DMA 迁移:从散装到 scatter-gather](01-ahci-dma.md)
