---
title: 003 · IBlockDevice 块设备抽象
---

# 003 · IBlockDevice 块设备抽象

> 立一个块设备接口 `IBlockDevice`,让 ext2 只对"一个能按块读写的设备"说话,从此接 NVMe、VirtIO 任何一种盘都不必动 ext2 本体——这是依赖倒置第一次在 Cinux 落地。

## 本章路线

- [01 · 问题在哪,与抽一个最小接口](01-problem-interface.md)
- [02 · ext2 解耦、抽象值在哪、验证](02-decouple-and-verify.md)
