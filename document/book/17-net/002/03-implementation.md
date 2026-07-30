---
title: 03 · 三件套、loopback 试验台与 e1000 接通
---

# 三件套、loopback 试验台与 e1000 接通

## 三件套:ARP / IPv4 / ICMP

协议按 ethertype 注册成三个 handler,单向依赖、无环:

- **ArpModule**(0x0806):收到 ARP 帧时,把「sender 的 IP→MAC」学进缓存,如果是请求且问的是自己就答一个 reply(从**同一块网卡**出,FOLD-B)。要发包时 `resolve_l3(ip)`:缓存命中返 MAC,miss 就发一个 ARP request 出去、返 false(异步——这轮没解析到,下轮 poll 再试,不阻塞)。
- **Ipv4Module**(0x0800):校验 IPv4 头(version/IHL/总长/头校验和,复用 `internet_checksum`),proto 字段是 ICMP 就交给 IcmpModule。要发包时建 IPv4 头 + 算校验和,以太网那跳走 ARP 解析下一跳(loopback 跳过 L2)。
- **IcmpModule**:收到 echo-request 就生成 echo-reply(整包 copy、type 改成 0、重算 ICMP 校验和、源/目的 IP 对调);收到 echo-reply 就记下 `reply_count`/`last_id`/`last_seq`(ping 发起方据此知道往返成了)。

这三个组合起来是单向的:ICMP 的回包要通过 IPv4 发,但这个回引是**逐调用**传 `Ipv4Module&`(作为 handler 的形参),不存成员——所以没有构造环。

### 校验和落地,与 Linux 的拆法对照

上一篇刚把反码和的算法(累加 + 进位回卷 + 最后取反)讲透,这里看它怎么落进三件套——IPv4 头、ICMP 都用它算校验和。Cinux 的实现在 `checksum.cpp` 里,核心是 `finalize_checksum` 那个折叠循环:

```cpp
while (partial_sum >> 16)
    partial_sum = (partial_sum & 0xFFFF) + (partial_sum >> 16);
```

这个循环干的正是上一篇说的「进位回卷」:只要部分和还溢出 16 位,就把溢出的高位折回低位加进去,直到塞得进 16 位为止。

同一套算法在 Linux 里被拆成了几个分开的原语,而且每个架构有自己的汇编实现——网络是热路径,值得为它单独手写:`csum_partial(buf, len, seed)` 返回一个 32 位未折叠的部分和,`csum_fold` 把它折叠成最终的 16 位值,`ip_fast_csum` 是 IPv4 头的快路径,`csum_tcpudp_magic` 把伪首部跟 payload 的部分和合并成 TCP/UDP 的校验和。Cinux 这边用一份通用 C++ 把「累加 + 折叠」合一,不分原语:正确性相同,工程力度不同。Linux 那套拆分不只是为性能——它还用 `__wsum`(部分和,可继续合并)和 `__sum16`(最终值,已折叠)两个类型,在编译期把「还能再合并」和「已经折叠完」两个阶段分开,让「把已折叠的值又当部分和去合并」这种误用编不过。Cinux 没要这层静态保护,靠人盯。

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
