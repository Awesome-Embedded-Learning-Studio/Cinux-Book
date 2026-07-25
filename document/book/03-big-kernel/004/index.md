---
title: 004 · PIC、IRQ 与 PIT:让内核听见时钟
---

# 004 · PIC、IRQ 与 PIT:让内核听见时钟

> 上一篇我们给内核装上了异常安全网,可 main 里有一句扎眼的警告——「现在千万别 sti,还没有 IRQ handler,PIT 中断一来就 Double Fault」。这一章兑现它:配上 8259A 双 PIC、挂上 PIT 定时器、注册好 IRQ handler,然后才敢按下 `sti`。内核从此第一次拥有「时间感」。

## 本章路线

- [01 · 导引:点亮什么、为什么、设计图](01-intro.md)
- [02 · PIC:把 16 条硬件中断挪出异常区](02-pic.md)
- [03 · IRQ 路由与 PIT:让时钟嘀嗒起来](03-pit-irq.md)
- [04 · 调试现场、验证与下一站](04-wrapup.md)
