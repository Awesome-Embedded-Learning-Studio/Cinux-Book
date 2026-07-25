---
title: 007 · PTY:伪终端把 console 单例变成多路终端
---

# 007 · PTY:伪终端把 console 单例变成多路终端

> 立 PTY(伪终端):一对 master/slave,slave 对程序表现得像真终端,master 是终端模拟器那一端。开一对就是开一个新终端,把 062 的 console 单例升级成 Linux 风格的多路 PTY。

## 本章路线

- [01 · PTY:伪终端把 console 单例变成多路终端](01-intro.md)
- [02 · 代码路线:PTY 核心、两条接缝、DevFS 节点与控制终端](02-implementation.md)
- [03 · 收尾:验证、没做的与小结](03-wrapup.md)
