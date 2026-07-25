---
title: 01 · 导引:点亮什么 + 还掉 L4 分派的债
---

# 导引:点亮什么 + 还掉 L4 分派的债

> 上一章(058)把协议栈立到了 `ping` 能通——以太网、ARP、IPv4、ICMP 全跑起来了。可那一章末尾留了句诚实话:「这一步只到 ICMP,TCP、UDP、socket API 都还没有」。不止没有——IPv4 收到一个包之后,**L4 分派是硬编码的**:`if (ip.proto == ICMP) icmp.handle(...)`,旁边还挂着一句 TODO「proto→handler table for UDP/TCP」。这一章加 UDP,顺手把这笔债还了:把 IPv4 的 L4 分派从「写死 ICMP」升级成一张 **proto→handler 表**(ICMP 自动迁进去,UDP 挂在同一个机制上),再立 UDP 这个协议——封装、伪首部校验和、端口多路复用。沿用上一章那套「底子优先」:先在 loopback 上把 UDP round-trip 端到端证明对,再去真 e1000 上发一发。
>
> punchline 是 UDP 数据报真的能往返——loopback 上一个 listener 收到 `send` 出去的包、校验和过、端口对、payload 对;e1000 上能把 UDP 包发到 QEMU 的 SLIRP 网关。一条诚实的边界先说在前头:这一章的 UDP 还**没有 socket API**(`socket`/`bind`/`recvfrom` 那一套留到后面)——验证靠内核侧的测试接口和 host 单测,不是用户态能 `socket()` 发 UDP。真 socket 留下一章的事;这一章把协议层做对。

## 这章咱们要点亮什么

1. **还掉 L4 分派的债**:IPv4 收到包按 `ip.proto` 字段查一张表分派(ICMP=1、UDP=17),不再写死。加 TCP 时挂同一个表,不疼。
2. **UDP 校验和的连续缓冲区法**:校验和覆盖「伪首部 + UDP 头 + payload」三段,把三段拼进一个连续缓冲区一把 `internet_checksum`,比「伪首部算个 partial 再分别累加」少一道弯。
3. **端口多路复用**:UdpModule 持一张小端口表,`bind`/`unbind`,收到的包按目的端口分派给 listener。
4. **底子优先复用**:UDP round-trip 先在 loopback 上确定性地证明对(镜像上一章的 `test_ping_loopback`),再上 e1000。

## 先还债:IPv4 的 L4 分派表

上一章 IPv4 收到包,L4 分派是这么写的(`ipv4.cpp` 旧版):

```cpp
if (ip.proto == kIpProtoIcmp) icmp_.handle(...);   // 硬编码 + 旁边一句 TODO
```

加 UDP 时面前两条路。一是「兄弟分支」——再加一个 `if (ip.proto == kIpProtoUdp) udp_.handle(...)`。看着省事,可它留了两套分派机制,那句 TODO 也更扎眼——以后加 TCP 再加一个 `if`,这套「每来一个协议加一个 if」的味道只会越来越重。二是正经做一个 **proto→handler 表**,对齐 Linux 的 `inet_protos`:一个协议号对应一个 handler,收到包查表分派。

选第二条。在 `ipv4.hpp` 立一个 `L4Handler` 接缝(签名跟上一章的 `ProtocolHandler` 同一层,只是它分派的是 L4 而非 ethertype):

```cpp
class L4Handler {
public:
    virtual void handle(const Ipv4Header& ip, FrameView payload, NetDevice& dev,
                        Ipv4Module& ipv4, NetStack& stack) = 0;
};
```

(`ipv4.hpp:99`。)`Ipv4Module` 持一张小表 `L4Slot l4_[kMaxL4=4]` + `add_l4(proto, handler)`(`ipv4.hpp:127`)。关键的一步:**ICMP 在构造函数里自动 `add_l4(kIpProtoIcmp, icmp)` 进表**(`ipv4.cpp:45`)——所以现有那一行 `Ipv4Module ipv4(icmp, &arp)` 一个字不用改,ICMP 就从「硬编码」迁到了「表里的一项」。UDP 挂同表:`ipv4.add_l4(kIpProtoUdp, udp)`。收包时查表分派,原来的 `if` 连同那句 TODO 一起删掉:

```cpp
for (uint32_t i = 0; i < kMaxL4; ++i) {
    if (l4_[i].h != nullptr && l4_[i].proto == ip.proto) {
        l4_[i].h->handle(ip, FrameView(l4, l4_len), dev, *this, stack);
    }
}
```

(`ipv4.cpp:84`。)这一步有个不大不小的编译坑值得记。构造函数里 `add_l4(kIpProtoIcmp, icmp)` 要把 `IcmpModule&` 转成 `L4Handler&`(派生→基类),这需要 `IcmpModule` 的**完整类型**可见。可 `ipv4.hpp` 只对 `IcmpModule` 做了前向声明——不能 `#include "icmp.hpp"`,那样会循环(`icmp.hpp` 反过来要 `ipv4.hpp`)。解法是把构造函数的定义挪到 `ipv4.cpp`(.cpp 里 `#include "icmp.hpp"`,完整类型可见),头里只留声明。这是唯一一处不顺手的地方,但它是「想保持头文件无循环依赖」的必然代价。

> 这一步对已经验过的 ping 路径是改动,所以得有双保险:`test_ping_loopback` 和 `test_ping_e1000` 两条腿都打 ICMP,ICMP 迁表迁错了立刻红。实测两腿都还通(`[net] loopback ping reply`、`[net] e1000 ping 10.0.2.2 reply`),证明 ICMP 走新表跟走老 `if` 一样——还债没把路走窄。
