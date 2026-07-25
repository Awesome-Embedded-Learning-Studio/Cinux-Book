---
title: 003 · 跨进长模式
---

# 003 · 跨进长模式

> 在那张 32 位 PM 的地基上,搭一套**临时分页**、按 Intel 规定的固定顺序拨开几个开关,再用一句远跳跨进 64 位长模式——跨过去之后,debugcon 会再吐一个 `L`。

## 本章路线

- [01 · 点亮什么与为什么](01-intro.md)
- [02 · 设计图:临时页表与长模式状态机](02-design.md)
- [03 · 代码路线:setup_page_tables、enter_long_mode、扩展 GDT](03-implementation.md)
- [04 · 调试现场:页表、开关顺序、L/D 位、重定位](04-debug.md)
- [05 · 验证、下一站与参考](05-wrapup.md)
