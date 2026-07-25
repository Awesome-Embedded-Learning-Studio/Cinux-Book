---
title: 009 · init 线程:让启动流程能被调度、能阻塞
---

# 009 · init 线程:让启动流程能被调度、能阻塞

> 把启动流程本身重构成 init 线程——对齐 Linux 那套 boot 上下文 → idle、PID 1 的 init 线程接管一切的模型,让启动逻辑第一次能被调度、能阻塞。

## 本章路线

- [01 · 导引:启动逻辑不在任何可调度的线程上](01-intro.md)
- [02 · Linux init 模型对齐:boot、init、idle](02-model.md)
- [03 · 三个配套小修:instance、launch_first_user、main 清理](03-patches.md)
- [04 · 调试现场:重构激活了潜伏的虚拟地址碰撞](04-debug.md)
- [05 · 收尾:验证与下一站](05-wrapup.md)
