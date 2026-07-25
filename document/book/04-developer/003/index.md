---
title: 003 · HPET 与 RTC:给内核两种时间
---

# 003 · HPET 与 RTC:给内核两种时间

> monotonic 与墙钟是两种语义,要两个硬件源:HPET 做 free-running 计数器给单调纳秒,RTC 在 boot 读一次给 Unix epoch,接到 `sys_clock_gettime` 各贡献自己擅长的精度。

## 本章路线

- [01 · 导引:两种时间,两个源](01-intro.md)
- [02 · HPET 与 RTC 驱动实现](02-implementation.md)
- [03 · 验证与收尾](03-debug.md)
