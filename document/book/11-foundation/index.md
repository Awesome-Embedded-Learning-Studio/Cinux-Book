---
title: 基础设施（v1.0.0 弧）
---

# 基础设施（v1.0.0 回迁弧）

> 从这里开始,教程进入 **v1.0.0 回迁弧**:把 035（多终端）之后的 CinuxOS 主线特性,按时间序 patch-replay 回教学线。这一卷收的是**跨切面的地基**——Cinux-Base 公共类型库、`ErrorOr` 错误处理、块设备抽象、内核日志、DMA 池、调试可观测性。它们本身不给系统加用户可见功能,但后面每一弧（SMP、网络、musl、文件系统升级）都站它们肩上。
>
> 阅读顺序:036（Cinux-Base + ErrorOr）→ 037（RingBuffer/klog/DMA）→ 038（IBlockDevice 抽象）→ ……。机制与纪律见 `meta/v1.0.0-migration-roadmap.md`。
