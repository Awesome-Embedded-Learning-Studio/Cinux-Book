---
title: Lab 058 · IPv4 / ICMP / ping 验证
---

# Lab 058 · IPv4 / ICMP / ping 验证

> 对应 `document/book/17-net/058-ipv4-icmp-ping.md`。验证档 **A 档**:本章的 punchline 是 `ping 10.0.2.2` 真往返 QEMU SLIRP 网关(发 ICMP echo、收回 echo reply)。验证分两层——先看内核测试把栈在 loopback(确定性)和 e1000(真硬件)上都验过,再看解耦门禁(`check_net_decoupling`)把「栈不依赖驱动」这条规矩机器执行住。

## 目标

确认四件事:

1. **loopback ping** 通(`test_ping_loopback`:栈逻辑在确定性试验台上端到端对);
2. **e1000 ping 10.0.2.2** 通(`test_ping_e1000`:真硬件、真 SLIRP 往返);
3. **解耦门禁**绿(`check_net_decoupling`:`kernel/net/` 不 include 驱动头、NIC 适配器不调 `::instance()`);
4. 看清两条接缝(`NetDevice` 设备接缝、`ProtocolHandler` 协议接缝)和「底子优先」的分层。

## 步骤

### 1. loopback ping(确定性证明)

这一步验的是「栈逻辑对不对」,不碰硬件时序。`run-kernel-test` 默认挂 e1000 + loopback 都在:

```bash
cmake --build build --target run-kernel-test 2>&1 | grep -aE "test_net::|=== net" | head
```

应看到 `=== net ===` 段下四项全 PASS:`test_ping_loopback`(loopback ping 127.0.0.1,一个 poll 端到端)、`test_ping_e1000`(真 ping 10.0.2.2)、`test_production_ping`(生产栈)、`test_syscall_ping`(sys_ping syscall)。**`test_ping_loopback` 是「底子」被证明对的关键**——它在纯软件设备上完成 request→reply 往返,没有 sti/hlt、没有 SLIRP 时序,栈逻辑对才过。

### 2. e1000 ping 10.0.2.2(A 档 punchline)

真硬件往返。`test_ping_e1000` 在内核里发 ICMP echo 到 SLIRP 网关 10.0.2.2、等回复:

```bash
cmake --build build --target run-kernel-test 2>&1 | grep -aE "e1000 ping|reply id=" | head
```

应看到类似 `[net] e1000 ping 10.0.2.2: reply id=... seq=1`。这证明 ARP 解析(把 10.0.2.2 翻成网关 MAC)+ IPv4 包头 + ICMP echo 真发出去了、SLIRP 的 echo reply 真收回来了。**栈在 loopback 上对了,接 e1000 才一次过**——这就是「底子优先」的回报。最终 `949 passed, 0 failed` + ALL TESTS PASSED。

### 3. 解耦门禁(check_net_decoupling)

这条规矩——「`kernel/net/` 不许 include 驱动/DMA/arch-irq 头、NIC 适配器不许调 `::instance()` singleton」——有脚本盯着:

```bash
cmake --build build --target check_net_decoupling 2>&1 | tail -5
```

应看到 `[net-decouple] all invariants hold` + `Built target check_net_decoupling`。它跑的是 `scripts/check_net_decoupling.sh` 里的 grep:`kernel/net/` 一旦 `#include` 了 e1000/dma_buffer/irq 就 FAIL。想验它真能抓人,可以临时往 `kernel/net/net_types.hpp` 塞一行 `#include "kernel/arch/x86_64/irq.hpp"` 再跑——应报 `DECOUPLING VIOLATED`;删掉复绿(验完别忘了真删)。

### 4. 两条接缝(NetDevice / ProtocolHandler)

```bash
sed -n '34,65p' kernel/net/net_device.hpp
```

应看到 `class NetDevice` 的虚接口:`mac()->bool`、`has_ethernet_header()`、`max_frame()`、`supports_zerocopy()`、`poll_rx(Packet&)`、`send_l3(next_hop, ethertype, l3, len)`。注意 `send_l3` 收的是 **L3 数据**(IP 包),以太网头由设备自己决定补不补——这就是 loopback(不补)和 e1000(补)能走同一接口的原因。协议接缝 `ProtocolHandler` 按 ethertype 注册(0x0806=ARP、0x0800=IPv4),`NetStack::add_protocol` 挂上。

### 5. host 单测(协议层单元测试)

协议层有一组 host 单测(不进内核、不靠 QEMU,链 `net_stack.cpp` 在 host 上跑):

```bash
cmake --build build -j$(nproc) --target test_net_dispatch test_net_arp_cache test_net_checksum 2>&1 | grep -iE "Built target test_net|error" | head
```

应能编出 `test_net_dispatch`(派发命中 + L2 解析 + 回收合约)、`test_net_arp_cache`(ARP 缓存 insert/lookup/evict)、`test_net_checksum`(RFC1071 校验和向量)。这些是「栈是纯协议库、能在 host 单测」的证据——也是解耦的实惠。

## 验收清单

- [ ] `test_ping_loopback` PASS(栈逻辑在 loopback 上确定性证明对)。
- [ ] `test_ping_e1000` PASS,有 `e1000 ping 10.0.2.2: reply` 输出(真 SLIRP 往返);949/0。
- [ ] `check_net_decoupling` 绿(`all invariants hold`),知道它是 grep 门禁、能抓驱动头泄漏。
- [ ] `NetDevice`(`net_device.hpp:34`)是设备接缝,`send_l3` 收 L3 数据、L2 头设备自决——loopback/e1000 同接口。
- [ ] 知道只有 ICMP(没 TCP/UDP/socket),ping 是内核 syscall 代办(不是用户态组包)。

## 别做这些

- **别**跳过 loopback 直接在 e1000 上调栈——栈错了还是 SLIRP 时序错了会搅成一团。「底子优先」就是先把栈在 loopback 上证明对,再接硬件,失败才锁得死在适配器层。
- **别**往 `kernel/net/` 里 `#include` 驱动头(e1000/dma_buffer/irq)图省事——`check_net_decoupling` 会 FAIL。栈要碰硬件,走 NetDevice 接口,不直接抓驱动。
- **别**以为有 TCP/UDP/socket 了——这一步只有 ICMP(ping)。`Ipv4Module` 现在把 proto==ICMP 硬交给 IcmpModule,没有通用 proto 分发表。socket API 是后面的事。
- **别**指望用户态能自己 `socket()` 组包发 ping——`ping` 走专用 `sys_ping` syscall,内核代办 ARP+echo+等回复。用户态拿不到原始包。
- **别**忘了 ARP 是异步的——`resolve_l3` miss 时这一轮发不出包,得下轮 poll 收到 ARP reply 填缓存后再发。ping 循环跑两轮以上是正常的(先 ARP、再 echo),不是 bug。
