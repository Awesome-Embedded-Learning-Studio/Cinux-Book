---
title: 03 · 没做的与小结
---

# 没做的与小结

## 这章没做的

- **socket API**:`socket(AF_INET, SOCK_DGRAM)` / `bind` / `sendto` / `recvfrom` 那一整套用户态接口还没有。这一章的 UDP 是内核协议层,验证靠内核侧测试接口和 host 单测。真让用户态程序 `socket()` 发 UDP,是下一章(socket 适配器)的事。
- **TCP**:无连接的 UDP 做了,面向连接的 TCP(握手、序号、挥手)留更后面。
- **UDP echo round-trip on SLIRP**:SLIRP 没 UDP echo 服务,所以 e1000 那步只能 TX。真 UDP echo 要有 socket API + 真对端(或换个有 UDP echo 的网络环境)。
- **端口表扩容**:协议层 16 个槽够测试用,socket 层落地后再按需扩到几百。

## 小结

- 上一章 IPv4 的 L4 分派是硬编码 ICMP(挂一句 TODO),这一章还债:升级成 proto→handler 表,ICMP 自动迁入(ctor 注册),UDP `add_l4(17)` 挂同表。加 TCP 不疼。还债有双保险——两条 ping 腿都打 ICMP,迁错立刻红。
- UDP 校验和覆盖「伪首部 + UDP 头 + payload」三段,用连续缓冲区法拼一把 `internet_checksum`(自带 fold+complement),比 partial 累加少一道弯;0 发 0xFFFF(RFC 768)。
- 端口多路复用:`UdpModule` 持小端口表(16 槽),`bind`/`unbind` + listener 回调——这就是将来 `socket()`/`recvfrom()` 的地基。
- 沿用底子优先:loopback 单次 poll 跑完 round-trip(确定性,躲 e1000 RX 时序坑),再上 e1000 TX smoke(SLIRP 无 UDP echo,TX-only)。
- 这章只到协议层,socket API 留下一章、TCP 留更后面。
