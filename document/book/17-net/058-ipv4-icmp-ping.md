---
title: 058 · IPv4 / ICMP / ping:先在 loopback 上把栈证明对,再接真网卡
---

# 058 · IPv4 / ICMP / ping:先在 loopback 上把栈证明对,再接真网卡

> 上一章立起了 e1000 这块能收能发的网卡。这一章在它上面立网络协议栈——以太网解析、ARP(把 IP 翻成 MAC)、IPv4、ICMP——一直做到 `ping` 能通。但这一章真正要讲的不是「协议怎么填字段」,而是**怎么搭一个你能信得过的协议栈**:不是一上来就把包扔给真网卡 + QEMU 的 SLIRP 网关、在「我的栈对不对」和「时序对不对」两团迷雾里调试,而是**先在一个确定性的软件试验台(loopback)上把整条栈端到端证明对,再去接真硬件**。这么做有一个直接的回报:接 e1000 的时候,ping 一次就过——因为栈的逻辑已经在 loopback 上验过了,真出了问题只会锁死在「网卡适配器」那一层,不会变成「到底是栈错了还是时序错了」的玄学。A 档:验证的 punchline 是 `ping 10.0.2.2` 真往返 QEMU 的 SLIRP 网关(发 ICMP echo、收回 echo reply)。
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

## 三件套:ARP / IPv4 / ICMP

协议按 ethertype 注册成三个 handler,单向依赖、无环:

- **ArpModule**(0x0806):收到 ARP 帧时,把「sender 的 IP→MAC」学进缓存,如果是请求且问的是自己就答一个 reply(从**同一块网卡**出,FOLD-B)。要发包时 `resolve_l3(ip)`:缓存命中返 MAC,miss 就发一个 ARP request 出去、返 false(异步——这轮没解析到,下轮 poll 再试,不阻塞)。
- **Ipv4Module**(0x0800):校验 IPv4 头(version/IHL/总长/头校验和,复用 `internet_checksum`),proto 字段是 ICMP 就交给 IcmpModule。要发包时建 IPv4 头 + 算校验和,以太网那跳走 ARP 解析下一跳(loopback 跳过 L2)。
- **IcmpModule**:收到 echo-request 就生成 echo-reply(整包 copy、type 改成 0、重算 ICMP 校验和、源/目的 IP 对调);收到 echo-reply 就记下 `reply_count`/`last_id`/`last_seq`(ping 发起方据此知道往返成了)。

这三个组合起来是单向的:ICMP 的回包要通过 IPv4 发,但这个回引是**逐调用**传 `Ipv4Module&`(作为 handler 的形参),不存成员——所以没有构造环。

## loopback:确定性的软件试验台

`LoopbackDevice`(`loopback_device.hpp:30`)是个软件 NetDevice:`has_ethernet_header()` 返 false(裸发 L3,不包以太网头)、`send_l3` 把字节拷进一个 8 槽 FIFO 队列、`poll_rx` 把槽指针交出去(零拷贝 RX,dispatch 后由 BufferSink recycle 归还队列槽)。

关键细节是**发不重入**:`send_l3` 只入队,**下轮 poll 才派发**。为什么?因为 loopback 是软件,handler 在处理一个收到的帧时,完全可能 `send_l3` 发一个 reply(比如 ICMP 收到 request 要发 reply)。如果 send 立刻同步派发,就会在「处理当前帧的 buffer 还没还」的时候递归进 dispatch,把当前 buffer 打花。入队 + 下轮派发,就把这个重入拆开了——这也是为什么 ping 在 loopback 上要跑两三轮 poll(request 一轮、reply 一轮、空队 break)。

> **`LoopbackDevice` 有 ~12KB(8×1518 存储),内核 16KB 栈放不下**——测试里用 `static` 分配,不放栈上。这是踩过才记的:大对象别想当然往栈上塞。

loopback 上 ping 127.0.0.1,实测一轮 poll 就端到端通了:`icmp.send_echo_request(lo, 127.0.0.1, id=0xABCD, seq=1)` → poll 抽干 → `reply_count()==1, last_reply_id()==0xABCD, last_reply_seq()==1`。**没有 sti/hlt、没有 LAPIC、没有 SLIRP**——栈对就过。这就是「底子」被证明对的那一刻。

## 接 e1000:ping 10.0.2.2 一次过

栈在 loopback 上对了,L2 接真网卡 e1000。适配器是 `E1000NetDevice`(`drivers/net/e1000_net_device.hpp:29`,header-only),把 e1000 的收发包装成 NetDevice 接口:

- **copy RX**:`poll_rx` 把 e1000 DMA buffer 里的帧 copy 进适配器自己的 `rx_scratch_[1518]`,`sink=null`(recycle 是 no-op,不用 BufferSink)。
- `send_l3`:组 `{以太网头 || L3 数据}` 进 `tx_buf_[]`,交给 `ctrl_.send_packet` 发出去(e1000 再 copy 进它自己的 TX DMA ring,所以 `tx_buf_` 不必 DMA-able)。
- **构造注入 `E1000Controller&`**——**不**用 `::instance()` singleton(两块网卡的适配器共享同一 ring 会撞;解耦门禁的 grep 也强制这点)。
- **RX scratch 和 TX buf 分离**:handler 在 `on_frame` 里 `send_l3` 发 reply 不会打花在途的 RX 帧。

接好之后,`test_net::test_ping_e1000` 真去 ping QEMU SLIRP 网关 `10.0.2.2`:循环发 echo request(iter1 触发 ARP resolve miss→发 ARP request;iter2+ ARP 命中→发 ICMP echo)+ poll(sti/hlt 让 QEMU 主循环跑 + LAPIC 定时器唤醒,SLIRP 的 reply 落进 ring——复用上一章 e1000 那套时序,**绝不写 trap 循环 hack**)。实测:`[net] e1000 ping 10.0.2.2: reply id=0x1234 seq=1`。ARP 解析 + ICMP echo 真往返了 SLIRP 网关。

这就是「底子优先」兑现的地方:接 e1000 是**一次过**——如果失败,只会锁在适配器这一层(收发没接好),不会变成「栈还是时序」的玄学,因为栈已经在 loopback 上验对了。

## 用户能看到的那一面:ping 命令

`ping` 对用户是个 shell 命令(`user/programs/shell/cmd_ping.cpp`),但活儿是内核干的。命令通过 `sys_ping` syscall(`sys_ping.cpp`)进内核,内核调 `cinux::net::ping()`——在内核里完成 ARP 解析 + 发 ICMP echo + 轮询等回复,把结果(收到几个 reply)返回给用户态。所以这一步的 `ping` **不是**用户态自己组包发,是内核代劳;用户态只是「发个 syscall、拿回结果、打印」。完整的用户态网络栈(socket API、用户态组包)是后面的事。

## 诚实的边界

**只有 ICMP,没有 TCP/UDP。** 这一步的协议栈只处理 ICMP(ping 用的 echo)。TCP、UDP、端口多路复用都还没立——`Ipv4Module` 现在把 proto==ICMP 硬交给 IcmpModule,没有「按 proto 号查表」的通用机制(TCP/UDP 加进来时才补那张表)。

**没有 socket API。** `ping` 走的是一个专门的 `sys_ping` syscall,内核替你做完一切。用户态还不能 `socket(AF_INET, SOCK_RAW, ...)` 自己组包发——没有 per-socket 的收发环、没有 `bind`/`connect`/`recv`。那套是后面几章立 socket 的内容。

**没有路由 / DNS。** ping 的目的 IP 是直接给的(10.0.2.2),没有路由表(只能 ping 直连的 SLIRP 网关或 loopback),没有 DNS(不能 ping 域名)。网关之外的地址,这一步到不了。

**ARP 是异步的、ping 要重试。** `resolve_l3` miss 时发 ARP request 返 false,这一轮发不出包;得下一轮 poll 收到 ARP reply 填了缓存,再发 ICMP echo。所以 ping 的循环天然要「先触发 ARP、再发 echo」跑两轮以上——这不是 bug,是 ARP 异步解析的必然。

验证该看到什么,见配套 lab。下一章该把这条网络线再往前推——UDP/TCP、socket API,那是网络栈真正「像样」的部分。
