---
title: 03 · 诚实的边界
---

# 诚实的边界

**只有 ICMP,没有 TCP/UDP。** 这一步的协议栈只处理 ICMP(ping 用的 echo)。TCP、UDP、端口多路复用都还没立——`Ipv4Module` 现在把 proto==ICMP 硬交给 IcmpModule,没有「按 proto 号查表」的通用机制(TCP/UDP 加进来时才补那张表)。

**没有 socket API。** `ping` 走的是一个专门的 `sys_ping` syscall,内核替你做完一切。用户态还不能 `socket(AF_INET, SOCK_RAW, ...)` 自己组包发——没有 per-socket 的收发环、没有 `bind`/`connect`/`recv`。那套是后面几章立 socket 的内容。

**没有路由 / DNS。** ping 的目的 IP 是直接给的(10.0.2.2),没有路由表(只能 ping 直连的 SLIRP 网关或 loopback),没有 DNS(不能 ping 域名)。网关之外的地址,这一步到不了。

**ARP 是异步的、ping 要重试。** `resolve_l3` miss 时发 ARP request 返 false,这一轮发不出包;得下一轮 poll 收到 ARP reply 填了缓存,再发 ICMP echo。所以 ping 的循环天然要「先触发 ARP、再发 echo」跑两轮以上——这不是 bug,是 ARP 异步解析的必然。

验证该看到什么,见配套 lab。下一章该把这条网络线再往前推——UDP/TCP、socket API,那是网络栈真正「像样」的部分。
