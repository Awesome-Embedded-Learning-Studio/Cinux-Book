---
title: 001 · 第一次跳进 Ring 3:用户态与特权隔离
---

# 001 · 第一次跳进 Ring 3:用户态与特权隔离

> 用 `SYSRET` 一脚跨进 Ring 3,再让用户代码里的第一条 `cli` 撞墙弹回 `#GP`——`CS=0x001b` 与那句 `protection works!` 就是特权隔离成立的证据。

## 本章路线

- [01 · 跳进 Ring 3:用户态与特权隔离](01-intro.md)
- [02 · 代码路线:从 MSR 装配到 `#GP` 来源判定](02-implementation.md)
- [03 · 调试现场:三连 bug 互相掩盖 + QEMU 不持久化 SFMASK](03-debug.md)
- [04 · 收尾:验证、下一站与参考](04-wrapup.md)
