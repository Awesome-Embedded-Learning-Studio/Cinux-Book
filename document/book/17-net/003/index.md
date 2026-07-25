---
title: 003 · UDP:还掉 L4 分派的债,再立一个协议
---

# 003 · UDP:还掉 L4 分派的债,再立一个协议

> 上一章把协议栈立到了 `ping` 能通——以太网、ARP、IPv4、ICMP 全跑起来了。这一章加 UDP,顺手把 IPv4 的 L4 分派从「写死 ICMP」升级成一张 proto→handler 表,再立 UDP 这个协议。

## 本章路线

- [01 · 导引:点亮什么 + 还掉 L4 分派的债](01-intro.md)
- [02 · UDP 协议层、底子优先与验证](02-implementation.md)
- [03 · 没做的与小结](03-wrapup.md)
