---
title: 007 · poll / select
---

# 007 · poll / select

> 一个任务只能睡在一个 fd 的等待队列上——你想同时等键盘有输入、又等一个 socket 来数据,傻办法是开两个任务各阻塞一个 fd;聪明的办法是一次把 N 个 fd 交给内核,任一就绪(或超时)就让内核叫醒你。这就是 poll/select 这一层多路复用要干的事。

## 本章路线

- [01 · 导引:点亮什么](01-intro.md)
- [02 · 多路复用这一层:poll_core 干什么](02-poll-core.md)
- [03 · 就绪缝:poll_events 二合一虚方法](03-poll-events.md)
- [04 · 统一 park:防丢唤醒](04-park.md)
- [05 · 一个 poll 睡在 N 个队列上](05-multi-queue.md)
- [06 · poll vs select:两个 ABI 共享 poll_core](06-poll-select-abi.md)
- [07 · 验证、没做的、小结](07-wrapup.md)
