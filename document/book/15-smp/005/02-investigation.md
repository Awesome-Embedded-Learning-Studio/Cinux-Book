---
title: 02 · 定位:抽函数重构踩出必现 panic,锁定同一份 ctx 并发读写
---

# 定位:抽函数重构踩出必现 panic,锁定同一份 ctx 并发读写

## 一个抽函数的重构,踩出每次必现的 panic

故事从一个重构讲起。内核启动时有个二选一:GUI 模式启桌面、非 GUI 模式 fork 一个 shell。这两段一直是内联在 `kernel_init_thread` 里的(`#ifdef` 分叉)。有个重构把它们抽成一个共享的 `launch_userspace()` 接口——逻辑和内联版**完全等价**,单核跑测试全过。

然后跑双核 `make run -smp 2`:**每次必 panic**。

有意思的地方来了:把重构 `git stash` 退回内联版,`-smp 2` 干干净净(一次 panic 都没有);换回抽函数版,每次 panic。更进一步的对照——把抽出来的 `gui_worker` 线程体改成空死循环(只 `yield`,啥也不干),`-smp 2` **照样 panic**。

这三组对照实验把范围钉死了:元凶不是 `gui_worker` 干了什么,而是**「多了一个任务 + 双核并发」本身**。抽函数只是让时序差了几个时钟,正好踩进一个一直潜伏着的竞态窗口。内联版不是没这个 bug,是时序恰好绕开了它——这是 heisenbug 最磨人的地方:它一直在,只是没被触发。

## 定位:同一份 ctx 被两头并发读写

加几行诊断打印,在 `schedule()` 的 `context_switch` 前打出 cpu/prev_tid/next_tid,铁证就出来了:

```
cpu1 tid3(kernel_init) -> tid5(gui_worker)   # gui_worker 在 cpu1 跑
cpu1 tid5 -> tid3                              # gui_worker 让出 cpu1,cpu1 存它的 ctx
cpu0 tid4 -> tid5                              # cpu0 同时切入 gui_worker,恢复同一份 ctx  ← 竞态窗口
```

问题清楚了:任务从 cpu1 迁移到 cpu0 时,**cpu1 的 `context_switch` 正在保存 `task->ctx`,cpu0 的 `context_switch` 已经在恢复同一份 `task->ctx`**。同一个 ctx 字段被并发一读一写,写花。写花之后 `ret` 跳到垃圾 RIP(实测 `0x1B` / `0xffffffff8002018e`——后者不在内核地址段,证实是 ctx 毁了之后跳到的随机地址),触发 #UD/#BP。

runqueue 是有锁的(`irq_guard`),所以**不是 runqueue 竞态**——坏的是 task 的 ctx 字段本身。052 的多核安全有个假设:「`pick_next` 把任务从队列摘走,就不会有两个核同时跑同一个任务」。这个假设挡住了「两核同时**运行**同一任务」,但**漏了迁移窗口**:旧核还在存、新核已经开始取。摘出队列和「上下文存完」之间有个时间差,窗口就在这儿。
