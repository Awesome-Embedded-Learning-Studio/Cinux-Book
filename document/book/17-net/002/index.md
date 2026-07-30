---
title: 002 · IPv4 / ICMP / ping:先在 loopback 上把栈证明对,再接真网卡
---

# 002 · IPv4 / ICMP / ping:先在 loopback 上把栈证明对,再接真网卡

> 上一章立起了 e1000 这块能收能发的网卡。这一章在它上面立网络协议栈——以太网解析、ARP、IPv4、ICMP——一直做到 `ping` 能通。

## 本章路线

- [01 · 导引:底子优先与两条接缝](01-intro.md)
- [02 · 反码和:网络包怎么自己证明自己没坏](02-checksum.md)
- [03 · 三件套、loopback 试验台与 e1000 接通](03-implementation.md)
- [04 · 诚实的边界](04-wrapup.md)
