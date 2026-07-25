---
title: 002 · 进入保护模式
---

# 002 · 进入保护模式

> 和实模式告别——建出 Cinux 的第一张 GDT,拨动 `CR0` 上的一个开关,再用一句远跳,让 CPU 真正跨进 32 位保护模式。跨过去之后,BIOS 就再也叫不应了。

## 本章路线

- [01 · 点亮什么与为什么](01-intro.md)
- [02 · 设计图:扁平 GDT 与模式状态机](02-design.md)
- [03 · 代码路线:GDT、lgdt、CR0.PE、远跳、pm_entry](03-implementation.md)
- [04 · 调试现场:DS、远跳、译码宽度、#GP](04-debug.md)
- [05 · 验证、下一站与参考](05-wrapup.md)
