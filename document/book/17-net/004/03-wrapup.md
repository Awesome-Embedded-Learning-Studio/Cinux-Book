---
title: 03 · 没做的与小结
---

# 没做的与小结

## 这章没做的(很多,诚实列)

这一章砍掉的是 TCP 的「鲁棒性」那一半,需要 timer 基建,CinuxOS 这会儿没有:

- **重传 / RTO / 滑动窗口 / 拥塞控制**:这一章不丢包就工作,丢包没重传(对端不 ACK 就卡)。真 TCP 的可靠性全在这,要内核 timer-wake 基建。
- **TIME_WAIT**:主动关方 ACK FIN 后直接 Closed,不等 2MSD(要 timer)。
- **ISN 随机化**:用确定性递增计数器(测试可复现),随机化(防序号预测)留 follow-up,跟 ASLR 那套安全考虑同族。
- **乱序重组 / 重复 ACK / 快速重传**:乱序包直接丢,不缓存不重组。
- **TCP Socket API**:`listen`/`accept`/`recv`/`sendto` 没有——这一章是协议层,socket 层是下一章。
- **production net_init 接线**:这一章 test-only(没消费者),真接进 net_init 等有 socket 消费者。

## 小结

- TCP(协议号 6)挂进 063 立的 L4 表(`ipv4.add_l4(kIpProtoTcp, tcp)`),加协议不疼。
- TCP 比 UDP 多一台**连接状态机**:每个连接一个 TCB(state / 4-tuple / iss / snd_nxt / rcv_nxt),固定 8 槽,在握手→established→挥手间转换。状态机是 TCP 灵魂,也是篇幅所在。
- 核心算术:**SYN 占 1 序号、FIN 占 1 序号、数据占 len**;`snd_nxt` 是下个发的,`rcv_nxt` 是下个期望收的(=ACK 值)。三次握手协商序号,数据按序 + ACK 推进 rcv_nxt,四次挥手靠 FIN(占 1 序号)+ ACK 拆连接。
- 校验和**必填**(不像 UDP 可省),伪首部 proto=6 + 段,连续缓冲区一把算(同 UDP trick)。
- 底子优先:host 单测(NoL2Dev 捕获再 deliver,逐包断言 seq/ack)→ loopback 内核(poll budget loop 排干,4 polls 跑完握手+数据+挥手)→ e1000 TX。避重入(用 poll 拍扁,不在回调里发包)。
- 诚实边界:这是最小可用 TCP——重传/RTO/窗口/拥塞/TIME_WAIT/ISN 随机化/乱序重组都没做(要 timer 基建);socket API 留下一章。
