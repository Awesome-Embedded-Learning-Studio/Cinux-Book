---
title: Lab 003 · UDP 协议层验证
---

# Lab 003 · UDP 协议层验证

> 对应 `document/book/17-net/003/`。验证档 **A 档**:这一章交付的是 UDP 协议层 + IPv4 L4 分派表的还债。可这一章还没 socket API(留下一章),所以「用户态 socket() 发 UDP」做不到——验证靠内核侧的 loopback round-trip + e1000 TX smoke + host 单测,辅以「ICMP 走新表 ping 仍通」这个还债的双保险。

## 目标

确认六件事:

1. **L4 分派表真的还了债**:IPv4 收包按 `ip.proto` 查表分派,ICMP 不再硬编码;
2. **ICMP 迁进表没回归**:两条 ping 腿(loopback + e1000)还通;
3. **UDP 校验和是连续缓冲区法**:拼 [伪首部|UDP 头|payload] 一把 `internet_checksum`,0 发 0xFFFF;
4. **端口多路复用**:`UdpModule` 端口表 + `bind`/`unbind`;
5. **loopback round-trip 确定性**:单次 `poll()` 跑完 send→IPv4→L4 表→UdpModule→listener;
6. **e1000 UDP TX**:真 e1000 上 UDP 包发出去(SLIRP 不回,TX-only)。

## 步骤

### 1. host 单测:协议核心

```bash
./build/test/test_net_udp
```

应看到 `9 passed, 0 failed`。九个 case 罩的是 UDP 协议核心:UDP 头 round-trip、TX 线序(proto=17 + 端口 + 长度)、send→IPv4 表→handle→listener 完整 round-trip(用 no-L2 mock 把 send 捕获的 IP 包喂回,无 QEMU)、校验和损坏丢弃、`checksum=0` 接受、无 listener 丢弃、双端口 demux、unbind、重复 bind 拒。看一眼都测了啥:

```bash
grep -nE "void test_" test/unit/test_net_udp.cpp | head
```

应看到九个测试函数名,跟上面九个 case 一一对应。

### 2. 还债:IPv4 的 L4 分派表

```bash
sed -n '99,128p' kernel/net/ipv4.hpp
```

应看到 `class L4Handler`(虚 `handle`)+ `kIpProtoIcmp=1` / `kIpProtoUdp=17` + `add_l4(proto, handler)` + 内部 `l4_[]` 小表。再看 ICMP 怎么进表的:

```bash
sed -n '45,46p' kernel/net/ipv4.cpp
```

应看到构造函数里 `add_l4(kIpProtoIcmp, icmp)`——ICMP 自动注册进表(所以现有 `Ipv4Module ipv4(icmp, &arp)` 零改)。再看收包分派:

```bash
sed -n '84,86p' kernel/net/ipv4.cpp
```

应看到查表 `l4_[i].proto == ip.proto` 命中就 `handle`——原来的 `if (ip.proto == ICMP)` 没了,那句 TODO 也一起删了。

### 3. ICMP 迁表不回归(还债双保险)

ICMP 从硬编码迁进表,最怕的就是把 ping 走窄。两条 ping 腿是双保险:

```bash
cmake --build build --target run-kernel-test 2>&1 | grep -iE "ping reply|ping_loopback|ping_e1000" | head
```

应看到 `[net] loopback ping reply`、`[net] e1000 ping 10.0.2.2 reply`——ICMP 走新表跟走老 `if` 一样,ping 还通。

### 4. UDP 校验和:连续缓冲区法

```bash
sed -n '90,130p' kernel/net/udp.cpp
```

应看到 `send`:`HeapBuf buf(12 + sizeof(UdpHeader) + len)` 拼一个连续区 [伪首部 12 | UDP 头 8 | payload],`build_pseudo_header` + `build_udp_header`(checksum=0 计算时),`internet_checksum` 一把算,`cs == 0` 改 `0xFFFF`(RFC 768),嵌进 UDP 头,最后 `ipv4.send(... buf.p + 12 ...)` 发 UDP 部分(跳过伪首部,伪首部不上线)。那个 `HeapBuf` 是堆缓冲——UDP 包能到 ~1.5 KB,放内核栈上会爆(栈才 16 KB)。

### 5. 端口多路复用

```bash
sed -n '64,103p' kernel/net/udp.cpp
```

应看到 `bind(port, listener)`(登记哪个端口归谁听,满了/重复拒)、`unbind`(清槽)。再看表和回调:

```bash
sed -n '72,77p' kernel/net/udp.hpp
```

应看到 `on_udp(ip, src_port, payload)` 回调(payload 借用,handler 期间有效)+ `kMaxUdpPorts = 16`(协议层够用,socket 层落地后扩)。这套 `bind`/`unbind` + listener 就是将来 `socket()`/`recvfrom()` 的地基。

### 6. loopback round-trip(确定性证明)

```bash
grep -n "test_udp_loopback" kernel/test/test_net.cpp
```

应命中测试注册。跑一下看它打什么:

```bash
cmake --build build --target run-kernel-test 2>&1 | grep -iE "loopback UDP" | head
```

应看到 `[net] loopback UDP: 6 bytes from port 1234 (round-trip in one poll)`——单次 `poll()` 跑完整往返(send → IPv4 → L4 表 → UdpModule → listener),listener 收到、源端口对、长度对、payload 对。这就是「底子优先」的确定性证明:loopback 无 SLIRP 时序,躲开 e1000 RX 那个坑。

### 7. e1000 UDP TX(真硬件,SLIRP 不回)

```bash
grep -n "test_udp_e1000_tx" kernel/test/test_net.cpp
cmake --build build --target run-kernel-test 2>&1 | grep -iE "udp_e1000|udp.*e1000" | head
```

应看到 `test_udp_e1000_tx` 注册并 PASS。这一步在真 e1000 + SLIRP 上发 UDP 到 10.0.2.2,握手照搬 ping 的(ARP miss→request→SLIRP reply→cache hit),断言 `arp.lookup` 成功 + `udp.send` 被 IPv4 接受。**不指望 reply**——SLIRP 没 UDP echo 服务,这是 TX-only smoke,证 UDP 蹭通 ping 已验的 e1000 L2/L3 TX 路径。

两腿汇总:

```bash
cmake --build build --target run-kernel-test-all 2>&1 | grep -E "Tests: 9[0-9][0-9] passed" | tail -1
```

应看到单核 + `-smp 2` 两腿各 `969 passed, 0 failed`。

## 验收清单

- [ ] `./build/test/test_net_udp` 报 9 passed(协议核心 host 单测)。
- [ ] `ipv4.hpp:99` `L4Handler` + `add_l4`;`ipv4.cpp:45` ctor 注册 ICMP 进表;`:84` on_frame 查表分派(硬编码 `if` 删了)。
- [ ] ICMP 迁表 ping 不回归:`loopback ping reply` + `e1000 ping 10.0.2.2 reply` 仍在。
- [ ] `udp.cpp:90` `send` 用 HeapBuf 拼 [伪首部|UDP 头|payload],`internet_checksum` 一把算,0→0xFFFF,发 UDP 部分(伪首部不上线)。
- [ ] `udp.hpp:72` `on_udp` 回调 + `kMaxUdpPorts=16`;`udp.cpp:64` `bind`/`unbind`。
- [ ] `[net] loopback UDP: 6 bytes ... round-trip in one poll` 出现;`test_udp_e1000_tx` PASS;两腿 969/0。
- [ ] 知道这章**没 socket API**(留下一章),UDP 验证靠内核测试接口 + host 单测,不是用户态 socket()。

## 别做这些

- **别**指望用户态 `socket(AF_INET, SOCK_DGRAM)` 发 UDP——socket API(`socket`/`bind`/`sendto`/`recvfrom`)还没做,留下一章。这一章的 UDP 是内核协议层,listener 是内核侧注册的。
- **别**指望 e1000 那步收到 UDP reply——QEMU SLIRP 有 ICMP echo(所以 ping 收得回),但**没 UDP echo**,发出去的 UDP 不回。TX-only smoke 证的是「包能发出去」,不是「echo 往返」。真 UDP echo 要 socket API + 真对端。
- **别**以为 `checksum=0` 是校验和算错了——RFC 768 规定 0 是「发送方没算校验和」,接收方看到 0 跳过验证。发送方**算出** 0 要发 `0xFFFF`(因为 0 这个值被「无校验和」占了)。
- **别**把 16 个端口槽当限制——那是协议层测试够用的数,socket 层落地后会扩。也别以为端口表是 hash——它就是线性小表(16 项),协议层这点量级不值得上 hash。
- **别**以为 L4 表只有 4 项(`kMaxL4`)是 bug——IPv4 这一层能挂的协议本来就少(ICMP/UDP/TCP...),4 槽够当前。真不够再加,别 premature 优化。
