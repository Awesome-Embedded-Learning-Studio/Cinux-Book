---
title: 002 · RingBuffer / 内核日志 / DMA 池
---

# 002 · RingBuffer / 内核日志 / DMA 池

> 用刚接进来的 `Cinux-Base` 干三件越来越碍事的事:统一两套手写环形缓冲、给 `kprintf` 补一段可回放的历史、把散装 DMA 收成设备无关的池——三件事同病同方:别重复造轮子。

## 本章路线

- [01 · 导引:三件事,同一个毛病](01-intro.md)
- [02 · 环形缓冲归一 + 内核日志补记忆](02-ringbuffer-klog.md)
- [03 · DMA 池:收编散装 DMA 与 direct-map 那个坑](03-dma-pool.md)
