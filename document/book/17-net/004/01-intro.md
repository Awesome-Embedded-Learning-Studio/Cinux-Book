---
title: 01 · 导引:状态机与序号-ACK 算术
---

# 导引:状态机与序号-ACK 算术

> 063 把 UDP 立起来的时候,顺便把 IPv4 的 L4 分派做成了 proto→handler 表(ICMP=1、UDP=17),还了一句「加 TCP 不疼」。这一章兑现:把 TCP(协议号 6)挂进同一张表,立一个**最小可用的 TCP 协议层**——三次握手、序号-ACK、数据按序交付、四次挥手、RST。这一章真正要讲的不是「TCP 字段怎么填」(那跟 UDP 差不多),而是 TCP 比 UDP 多出来的那一大坨:**连接状态机**。UDP 发完就完(无连接),TCP 得先握手建连接、传输中维持序号纪律、完了挥手拆连接——这一整套状态转换是 TCP 的灵魂,也是这一章的篇幅所在。
>
> A 档:punchline 是 TCP 真能在 loopback 上端到端走通——握手 + 传一段数据 + 挥手,全在内核的 NetStack poll 里确定性跑完(不用真 timer、不用 SLIRP)。一条诚实的边界先说在前头,而且这条边界很大:**这是最小可用 TCP,不做重传、不做 RTO、不做滑动窗口、不做拥塞控制、不做 TIME_WAIT、不做 ISN 随机化**。这些是 TCP 的「鲁棒性」那一半,需要内核 timer 基建(CinuxOS 这会儿没有),留作后续。这一章做的是 TCP 的「正确性骨架」——在一切顺利(不丢包、不乱序)的前提下,握手/数据/挥手的协议逻辑对。TCP socket API(`listen`/`accept`/`recv`)也还没有,留下一章。

## 这章咱们要点亮什么

1. **TCP 挂进 063 立的 L4 表**:跟 UDP 一样 `ipv4.add_l8(kIpProtoTcp, tcp)`,加协议不疼。
2. **连接状态机是 TCP 的灵魂**:UDP 无状态,TCP 每个连接是一个 TCB(传输控制块),带着 state / 序号 / 对端地址,在握手→established→挥手的状态间转换。
3. **序号-ACK 算术的核心不变量**:SYN 占 1 个序号、FIN 占 1 个序号、数据占 len 个;`snd_nxt` 是下个要发的,`rcv_nxt` 是下个期望收的(也就是本端 ACK 的值)。
4. **三次握手 + 四次挥手**:握手建连接(SYN/SYN-ACK/ACK)、挥手拆连接(FIN/ACK 四步),靠序号-ACK 箐合。
5. **TCP 校验和必填**(不像 UDP 可以「无校验和」):伪首部 proto=6 + 段,连续缓冲区一把算,同 UDP 那个 trick。
6. **底子优先**:跟 058/063 一样,先 host 单测(确定性 NoL2Dev 逐步断言每段的 flags/seq/ack)→ loopback 内核端到端 → e1000 TX 收尾。

## TCP 比 UDP 多了什么:连接状态机

先回顾 UDP(063):无连接,发一个数据报就完,不维护任何对端状态。TCP 完全相反——它是**面向连接**的:传数据之前先握手「建连接」,传的时候维护「这个连接进行到哪了」,完了挥手「拆连接」。这个「连接」在内核里就是一个 **TCB(传输控制块)**,这一章叫 `Connection`(`tcp.hpp:215`):

```cpp
struct Connection {            // 一条连接的状态(state==kClosed 标空槽)
    TcpState  state;           // Closed/SynReceived/Established/FinWait1/...
    uint16_t  local_port;
    Ipv4Addr  remote_addr;
    uint16_t  remote_port;
    uint32_t  iss;             // 本端的初始序号
    uint32_t  snd_nxt;         // 下个要发的序号
    uint32_t  rcv_nxt;         // 下个期望收的序号(= 本端 ACK 的值)
    TcpListener* listener;
};
```

固定表 `cons_[kMaxTcpCons=8]`,4-tuple 键(local_port, remote_addr, remote_port)。监听端口另走 `listens_[8]` 表。状态枚举(`tcp.hpp:112`)覆盖握手到挥手的各阶段:Closed / SynReceived / Established / FinWait1 / FinWait2 / CloseWait / LastAck。

> 这一套状态转换就是 RFC 793 的 TCP 状态机(的子集)。TCP 的复杂度几乎全在这台状态机里——握手、数据、挥手、RST 各对应状态机的一段。这一章做的是「最小可用」子集:不处理同时打开、不处理 SYN 重传、不处理 TIME_WAIT(那是拆连接后的 2MSD 等待,要 timer)。把这些砍掉,剩下的状态机刚好够「顺利情况下」跑通一次连接的生命周期。

## 序号-ACK:TCP 的核心算术

TCP 一切靠序号和确认。理解这一章,先抓住这条不变量:**SYN 占 1 个序号、FIN 占 1 个序号、数据占 len 个序号**(纯 ACK 不占)。每个连接有两个序号在走:

- `snd_nxt` —— 我**下个要发**的序号。
- `rcv_nxt` —— 我**下个期望收**的序号,也就是我在 ACK 里写的值。

三次握手把这两个序号协商好(`tcp.cpp` 握手段):

1. **主动开(connect)**:我选个初始序号 `iss = next_isn()`,发 SYN(`seq=iss`),`snd_nxt = iss+1`(SYN 占了 1 个序号)。
2. **收 SYN-ACK**:对端回 `ack == iss+1`(确认我的 SYN),`seq = 对端的 iss`。我设 `rcv_nxt = seg.seq + 1`(对端的 SYN 占 1),发 ACK(`seq=snd_nxt, ack=rcv_nxt`),进 ESTABLISHED。
3. **被动开(收 SYN 到监听端口)**:反过来,我发 SYN-ACK(`seq=我的iss, ack=对方的seq+1`),进 SynReceived;第 3 个 ACK(`ack==iss+1`)来了就 ESTABLISHED,回调 `on_accept`。

> `next_isn()` 给每个连接一个初始序号——这一章用的是**确定性递增计数器**(起 0x4000,每连接 +64K,让不同连接的序号空间在测试里不重叠)。真 TCP 该用**随机 ISN**(防序号预测攻击,跟 F9 ASLR 同族的安全考虑),但随机化让测试不可复现,留 follow-up。这是个诚实的简化:协议逻辑对,安全性留后。

数据传输沿用这套算术:数据段 `PSH|ACK`,严格按序收(`seg.seq == rcv_nxt` 才收,乱序丢——不缓存、不重组,最小可用),收了就推进 `rcv_nxt += len`、回 ACK。挥手时 FIN 占 1 个序号,同样的 ACK 机制把它确认掉。
