---
title: Lab 069 · TCP 协议层验证
---

# Lab 069 · TCP 协议层验证

> 对应 `document/book/17-net/069-tcp.md`。验证档 **A 档**:这一章交付的是最小可用 TCP 协议层(握手/序号/数据/挥手/RST)。punchline 是 loopback 上 TCP 端到端走通——握手 + 数据 + 挥手在内核 poll 里确定性跑完。验证沿用 063 UDP 的底子优先:host 单测(逐步断言 seq/ack)→ loopback 内核 → e1000 TX。诚实边界:这是最小可用 TCP,重传/RTO/窗口/拥塞/TIME_WAIT/socket 都没做。

## 目标

确认七件事:

1. **TCP 挂进 L4 表**(proto 6,跟 ICMP/UDP 同表);
2. **连接状态机 + TCB**(state/4-tuple/iss/snd_nxt/rcv_nxt,固定 8 槽);
3. **序号-ACK 算术**(SYN/FIN 各占 1、数据占 len);
4. **校验和必填**(伪首部 proto=6,连续缓冲区,同 UDP trick);
5. **三次握手 + 四次挥手** + RST;
6. **loopback 内核 4 polls 跑完握手+数据+挥手**;
7. **e1000 TCP TX smoke**(SLIRP 不回,只 TX)。

## 步骤

### 1. host 单测:逐包断言 seq/ack

```bash
./build/test/test_net_tcp
```

应看到 `15 passed, 0 failed`。用 NoL2Dev 捕获 send_l3 不回环,手动把捕获的包喂回 rx 驱动下一步——于是握手每一段的 flags/seq/ack 都能逐包断言:头 round-trip、校验和门、proto 6 派发、3-way 握手(seq=iss / SYN-ACK ack=iss+1 / ACK)、RST、重复 4-tuple 拒、数据 round-trip、4-way 挥手,加对抗加固负测。「捕获再 deliver」是 TCP 测试的关键(真回环有时序,难断言具体某一段)。

### 2. 连接状态机 + TCB

```bash
sed -n '112,120p' kernel/net/tcp.hpp
sed -n '205,235p' kernel/net/tcp.hpp
```

应看到 `enum class TcpState`(Closed/SynReceived/Established/FinWait1/FinWait2/CloseWait/LastAck)+ `struct Connection`(TCB:state/local_port/remote_addr/remote_port/iss/snd_nxt/rcv_nxt/listener)+ `cons_[kMaxTcpCons=8]` 固定表 + `listens_[8]`。这就是 TCP 比 UDP 多出来的那一坨——每条连接一个 TCB,在握手→established→挥手间转换。

### 3. 序号-ACK 算术

```bash
sed -n '129,160p' kernel/net/tcp.cpp
sed -n '210,240p' kernel/net/tcp.cpp
```

应看到 `next_isn()`(确定性递增 ISN,起 0x4000 +64K/连接;随机化留 follow-up)+ 主动开(`iss=next_isn()`,发 SYN,`snd_nxt=iss+1`)。核心不变量:**SYN 占 1 序号、FIN 占 1 序号、数据占 len**;`snd_nxt`=下个发,`rcv_nxt`=下个期望收(=ACK 值)。握手就是协商这两个序号。

### 4. 校验和必填(send_segment)

```bash
sed -n '161,199p' kernel/net/tcp.cpp
```

应看到 `send_segment`:连续区 [伪首部 12(proto=6) | TCP 头 20 | payload] 一把 `internet_checksum`(同 UDP trick,HeapBuf 避栈爆),发 TCP 部分(跳伪首部)经 `ipv4.send(proto=6)`。注意 TCP 校验和**必填**(不像 UDP 可填 0 表示「无校验和」),入站有校验和门,过不了直接丢。

### 5. 挂进 L4 表

```bash
grep -n "add_l8(kIpProtoTcp\|kIpProtoTcp" kernel/test/test_net.cpp kernel/net/ipv4.hpp | head
```

应看到 `ipv4.add_l8(kIpProtoTcp, tcp)`——「TCP joins ICMP/UDP in the L4 table」(063 立的单一分派机制,加协议不疼)。

### 6. loopback 内核端到端(4 polls)

```bash
cmake --build build --target run-kernel-test 2>&1 | grep -iE "loopback TCP|e1000 TCP" | head
```

应看到 `[net] loopback TCP: handshake + 6-byte data + teardown in 4 polls (conn closed)` ——内核里一个 TcpModule 持 client+server 两条 conn(一表跑双端),靠 `NetStack::poll()` 的 budget loop(64)排干所有包,握手+数据+挥手在 4 次 poll 里确定性跑完,无 SLIRP/timer。还有 `[net] e1000 TCP TX -> 10.0.2.2: ARP resolved + SYN send ok (no reply expected)`(SLIRP 不回 TCP 握手,TX-only smoke,证 TCP 蹭通 ping/UDP 已验的 e1000 TX 路径)。

两腿汇总:

```bash
cmake --build build --target run-kernel-test-all 2>&1 | grep -E "Tests: 9[0-9][0-9] passed" | tail -1
```

应看到单核 + `-smp 2` 两腿各 `999 passed, 0 failed`(997 基线 + 2 TCP 内核测)。

## 验收清单

- [ ] `./build/test/test_net_tcp` 报 15 passed(逐包断言 seq/ack + 对抗加固负测)。
- [ ] `tcp.hpp:112` TcpState 枚举(握手→established→挥手各阶段);`:205` Connection(TCB);`:234` `cons_[8]`。
- [ ] `tcp.cpp:129` `next_isn`(确定性 ISN);握手段 SYN/FIN 各占 1 序号、`snd_nxt`/`rcv_nxt` 算术对。
- [ ] `tcp.cpp:161` `send_segment` 连续缓冲区校验和(proto=6,必填,HeapBuf)。
- [ ] `test_net.cpp` `ipv4.add_l8(kIpProtoTcp, tcp)`(挂 L4 表,加 ICMP/UDP 同表)。
- [ ] `[net] loopback TCP: ... 4 polls (conn closed)` + `[net] e1000 TCP TX ...`;两腿 999/0。

## 别做这些

- **别**指望 TCP 丢包还能工作——这是最小可用,**无重传/RTO**。这一章假设不丢包、不乱序,协议逻辑对;丢包了对端不 ACK 就卡。真 TCP 的可靠性(重传/窗口/拥塞)要内核 timer 基建,留 follow-up。
- **别**以为有 TCP socket API——`listen`/`accept`/`recv`/`sendto` 没有,这一章是协议层,socket 层是下一章。验证靠内核测试接口 + host 单测,不是用户态 socket()。
- **别**在 on_close 回调里调 close()——重入是 TCP 头号麻烦(处理一个包会触发发下一个包)。用 poll budget loop 把它拍扁成「一轮处理一轮」,测试显式分 poll 驱动四次挥手,比在回调里递归发包干净。
- **别**以为 ISN 是随机的——这一章用确定性递增计数器(测试可复现)。真 TCP 该随机 ISN(防序号预测,对齐 F9 ASLR),留 follow-up。
- **别**指望 e1000 那步收到 TCP 握手回复——SLIRP 不回 TCP(只回 ICMP echo)。TX-only smoke 证「SYN 能发出去」,不是「握手往返」。真 TCP echo 要 socket API + 真对端。
- **别**以为乱序包会被缓存重组——最小可用:乱序直接丢(严格按序收,`seg.seq==rcv_nxt` 才收),不缓存不重组。重复 ACK / 快速重传都没做。
