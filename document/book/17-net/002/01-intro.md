---
title: 01 · 导引:底子优先与两条接缝
---

# 导引:底子优先与两条接缝

> 上一章立起了 e1000 这块能收能发的网卡。这一章在它上面立网络协议栈——以太网解析、ARP(把 IP 翻成 MAC)、IPv4、ICMP——一直做到 `ping` 能通。但这一章真正要讲的不是「协议怎么填字段」,而是**怎么搭一个你能信得过的协议栈**:不是一上来就把包扔给真网卡 + QEMU 的 SLIRP 网关、在「我的栈对不对」和「时序对不对」两团迷雾里调试,而是**先在一个确定性的软件试验台(loopback)上把整条栈端到端证明对,再去接真硬件**。这么做有一个直接的回报:接 e1000 的时候,ping 一次就过——因为栈的逻辑已经在 loopback 上验过了,真出了问题只会锁死在「网卡适配器」那一层,不会变成「到底是栈错了还是时序错了」的玄学。验证的 punchline 是 `ping 10.0.2.2` 真往返 QEMU 的 SLIRP 网关(发 ICMP echo、收回 echo reply)。
>
> 一条诚实的边界先说在前头:这一步只到 **ICMP**(ping 用的协议)。TCP、UDP、socket API 都还没有——`ping` 是内核侧的一个 syscall(内核替你做 ARP 解析 + 发 echo + 等回复),不是用户态能 `socket()` 那种完整网络栈。那是后面几章的事。

## 这章咱们要点亮什么

1. **底子优先的打法**:栈先在 loopback 上端到端证明对,再接真网卡——把「栈逻辑对不对」和「硬件时序对不对」拆开调试。
2. **两条接缝**:NetDevice(设备接缝,L2,网卡实现)和 ProtocolHandler(协议接缝,L3,按 ethertype 注册)——栈和驱动之间只靠这两道抽象耦合,`kernel/net/` 里一行驱动头都不 include。
3. **ARP / IPv4 / ICMP 三件套**:ARP 把目的 IP 翻成下一跳 MAC、IPv4 包头 + 校验和、ICMP echo request/reply。
4. **loopback 怎么做到确定性**:软件设备,发即入队、下轮 poll 才派发(避开「handler 里 send 打花当前 buffer」的重入)。
5. **解耦门禁**:一个 grep 脚本 `check_net_decoupling.sh` 把「栈不许 include 驱动头」变成可机器执行的检查,不是嘴上说说。

## 底子优先:为什么先在 loopback 上证明

先说这一章的方法论,因为它比任何一行代码都重要。要做 `ping`,最直白的路是:把 ARP/IPv4/ICMP 写出来,直接挂到 e1000 上,`ping 10.0.2.2`,看通不通。不通就调。**这条路是雷区**——因为 e1000 收发要靠 QEMU SLIRP 的时序(上一章那个「必须读 RDH 把模拟器主循环踢一轮」的坑),一旦 ping 不通,你根本分不清是「我的 IPv4 校验和算错了」还是「SLIRP 的回包没投递进 ring」——两团迷雾叠在一起,定位起来纯靠猜。

这一章换打法:**先在 loopback 上把栈跑通**。loopback 是个软件设备(下面讲),发出去的包直接进它自己的队列、下轮 poll 取出来派发——不经过任何硬件、不依赖任何 QEMU 时序。在它上面 `ping 127.0.0.1`,请求和回复在一个 `poll()` 里就完成端到端往返(发 echo request → 入队 → poll 取出 → IPv4 解析 → ICMP 收到 request 生成 reply → 入队 → poll 取出 → ICMP 收到 reply 记下来)。**全程没有 sti/hlt、没有 LAPIC 定时器、没有 SLIRP**——栈逻辑对就过,错就立刻断在哪一层。

栈在 loopback 上对了,再接 e1000:ping 10.0.2.2 一次就过(后面会看到实测)。这就是「底子优先」的回报——把「栈对不对」和「时序对不对」解耦,各验各的。

## 两条接缝:NetDevice 和 ProtocolHandler

栈和驱动之间,立两道抽象。第一道是**设备接缝** `NetDevice`(L2,`net_device.hpp:34`)——网卡实现它,栈通过它收发,栈永远不直接碰 e1000 的寄存器:

```cpp
class NetDevice {
public:
    virtual bool     mac(EthAddr& out) const = 0;           // 有没有 MAC(loopback 没有)
    virtual bool     has_ethernet_header() const { return true; }  // 要不要 L2 头(loopback 不要)
    virtual uint32_t max_frame() const { return 1518; }      // MTU(loopback 65536)
    virtual bool     supports_zerocopy() const { return false; }  // 诚实声明能力(未来 virtio 才 true)
    virtual bool     poll_rx(Packet& out) = 0;               // 取一个收到的帧
    virtual ErrorOr<void> send_l3(const EthAddr& next_hop, uint16_t ethertype,
                                  const uint8_t* l3, uint32_t len) = 0;  // 发一个 L3 包
};
```

注意几个设计:`mac()->bool`(loopback 没 MAC,返回 false)、`has_ethernet_header()`(loopback 不带以太网头,设备自己决定 L2 帧长什么样)、`send_l3` 收的是 **L3 数据**(IP 包),要不要前面补以太网头由设备自己定(e1000 补、loopback 不补)。这让 loopback 和 e1000 走**同一个接口**,只是各自的 L2 处理不同。

第二道是**协议接缝** `ProtocolHandler`(L3)——按 ethertype 注册:0x0806 是 ARP、0x0800 是 IPv4。帧进来,NetStack 按 ethertype 派发给对应的 handler。

把这两道缝捏到一起的是 `NetStack`(`net_stack.hpp`)——唯一的前端,持一张设备表(从第一天就支持多块网卡共存,`kMaxDevs=3`,ARP reply 从同一块网卡出,栈里没有 singleton)和一张 ethertype→handler 派发表。它的 `poll()` 是预算制抽干:从每块设备各取一帧、解析 L2、按 ethertype 派发、用 scope_guard 回收 buffer,跑固定预算轮(`kPollBudget`)防 runaway。

> **解耦是硬规矩,有脚本盯着。** `kernel/net/` 里**一行驱动头都不许 include**(不能 `#include "e1000*.hpp"`、不能 `dma_buffer.hpp`、不能 `irq.hpp`)。这不是写在注释里的口头约束,是 `scripts/check_net_decoupling.sh` 这个脚本用 grep 机器执行的门禁:`grep -rnE '#include.*(e1000|dma_buffer|irq)' kernel/net/` 一旦命中就 FAIL。它还负测过——临时往 `kernel/net/` 注入一个 `#include "kernel/arch/x86_64/irq.hpp"`,门禁立刻报 `DECOUPLING VIOLATED`;删掉复绿。真能抓人。这道门禁保证「栈是纯协议库」——能在 host 上单测链接、零内核依赖、加新网卡不动栈。
