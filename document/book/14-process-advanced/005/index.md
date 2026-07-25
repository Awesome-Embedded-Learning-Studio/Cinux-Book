---
title: 005 · Pipe 增强与命名 FIFO
---

# 005 · Pipe 增强与命名 FIFO

> 匿名 pipe 早就有,可它有两个硬伤一直留着:阻塞用 sti/hlt 自旋实现(定时炸弹)、只能亲缘进程用。这一章把两件事都修了:阻塞改成真调度等待队列、加 O_NONBLOCK、写已关读端触发 SIGPIPE,再立命名 FIFO——给 pipe 一个名字,任意进程按名字 open 两端。

## 本章路线

- [01 · 导引:点亮什么](01-intro.md)
- [02 · 阻塞、SIGPIPE、命名 FIFO 三件实现](02-implementation.md)
- [03 · 验证、没做的、小结](03-wrapup.md)
