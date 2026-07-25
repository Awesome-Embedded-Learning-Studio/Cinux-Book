---
title: 02 · 校验和、挥手、RST 与底子优先验证
---

# 校验和、挥手、RST 与底子优先验证

## 校验和:必填,连续缓冲区法

跟 UDP 一样,TCP 校验和覆盖「伪首部(12 字节,只是 proto 字段填 6)+ TCP 头 + payload」,拼进一个连续缓冲区一把 `internet_checksum`(`tcp.cpp:172` 的 `send_segment`)。但有个差别:**UDP 的校验和可以省略**(发送方填 0 表示「没算」,RFC 768),**TCP 的校验和是必填的**——没有「无校验和」这个选项。所以入站有个校验和门(`verify_internet_checksum`),过不了直接丢,状态机不跑。

```cpp
// send_segment:连续区 [伪首部 12 | TCP 头 20 | payload],一把 internet_checksum,发 TCP 部分
HeapBuf buf(12 + sizeof(TcpHeader) + len);
build_pseudo_header(buf.p, ..., kIpProtoTcp, ...);
// ... 填 TCP 头(校验和字段=0 算) + payload ...
uint16_t cs = internet_checksum(buf.p, 12 + sizeof(TcpHeader) + len);
// 嵌进 TCP 头,发 TCP 部分(跳伪首部)经 ipv4.send(proto=6)
```

这套跟 UDP 的 send 一模一样的 trick(连续缓冲区避 fold/complement 衔接、HeapBuf 避栈爆),只是 proto 从 17 换成 6、校验和从「可省」变「必填」。

## 四次挥手 + RST

数据传完要拆连接,四次挥手(`tcp.cpp:254` 的 `close()` 入口,FSM 在 `handle()` 的 `kFinWait1` case `tcp.cpp:414` 起):主动方发 FIN(`seq=snd_nxt`)进 FinWait1 → 对端 ACK 进 FinWait2 → 对端发 FIN(它那边也 CloseWait → LastAck)→ 主动方 ACK,连接 Closed。FIN 占 1 个序号,跟数据一样靠 ACK 确认。这一章不做 TIME_WAIT(主动方 ACK FIN 后直接 Closed,不等 2MSD——那要 timer)。

RST 是「强制拆连接」。两种:SYN 到一个没人监听的端口,回 RST|ACK(`seq=0, ack=SEG.SEQ+1`,RFC 793);任意状态收到入站 RST,连接直接置 Closed。

## 底子优先:host 单测 → loopback 内核 → e1000

验证沿用 058/063 的「底子优先」,而且 TCP 这里特别有用——TCP 是多包交换(握手 3 包 + 数据 + 挥手 4 包),时序复杂,直接上真网卡 + SLIRP 调试会迷路。

**第一层:host 单测,逐步断言每一段。** `test/unit/test_net_tcp.cpp` 用一个 NoL2Dev(捕获 send_l3 不回环),手动把捕获的包喂回 rx 驱动下一步——于是握手每一段的 flags/seq/ack 都能逐包断言:SYN(seq=iss)、SYN-ACK(ack=iss+1)、ACK(ack=对端iss+1)……数据段、挥手段同理。这套「捕获再 deliver」的确定性是 TCP 测试的关键(真回环有时序,难断言具体某一段)。十五个 case(九个主流程 + 六个 adversarial):头 round-trip、校验和门、proto 6 派发、3-way 握手 seq-ack、RST、重复 4-tuple 拒、数据 round-trip、4-way 挥手,加六个对抗性(截断头、data_off 越界、data_off 下溢、连接表溢出、野 ACK、乱序数据)。

**第二层:loopback 内核端到端。** 内核里一个 TcpModule 持 client+server 两条 conn(一表跑双端),靠 `NetStack::poll()` 的 budget loop(64)排干所有包——握手 + 数据 + 挥手在 4 次 poll 里确定性跑完,日志打 `[net] loopback TCP: handshake + 6-byte data + teardown in 4 polls`。无 SLIRP、无 timer,纯靠 poll loop。

**第三层:e1000 TX smoke。** 真 e1000 上发 TCP SYN 到 10.0.2.2(SLIRP 不回 TCP 握手,所以只 TX):`[net] e1000 TCP TX -> 10.0.2.2: ARP resolved + SYN send ok`,证 TCP 蹭通 ping/UDP 已验的 e1000 L2/L3 TX 路径。

> 这里有个测试设计的小讲究:**避重入**。`on_close` 回调里不能调 `close()`(那会在 handle 过程中又触发发包,跟「handle 后置 state 写」「ACK/FIN 入队顺序」两个坑撞上),测试显式分 poll 驱动四次挥手。TCP 这种「处理一个包会触发发下一个包」的协议,重入是头号麻烦——用 poll budget loop 把它拍扁成「一轮处理一轮、下轮接着」,比在回调里递归发包干净得多。

## 验证

四层(沿用 058/063 分层)。

**第一层:host 单测。** `./build/test/test_net_tcp` 十五个 case(九个主流程 + 六个 adversarial,上面列过)。

**第二层:loopback 内核。** `test_tcp_loopback` / `test_tcp_e1000_tx` 在内核跑,两腿打 `[net] loopback TCP: ... in 4 polls` 和 `[net] e1000 TCP TX ...`。

**第三层:回归。** `check_net_decoupling`(网络层解耦不变量)绿。

**第四层:全量。** `run-kernel-test-all` 两腿各 999 passed / 0 failed(997 基线 + 2 TCP 内核测),AP 回读 PASS。
