---
title: 02 · UDP 协议层、底子优先与验证
---

# UDP 协议层、底子优先与验证

## UDP 协议层:头、校验和、端口

L4 表就位,UDP 挂上去。UDP 比 ICMP 简单——无连接、不保序、不重传,头就 8 字节(源端口、目的端口、长度、校验和)。真正要花心思的是**校验和**和**端口多路复用**。

**校验和:连续缓冲区法。** UDP 的校验和覆盖的不止 UDP 自己,还有一段 12 字节的**伪首部**(源 IP、目的 IP、协议号、UDP 长度)——这是为了让接收方顺便校验「这个包确实是从这个 IP 来、到这个 IP 去」,防止 IP 头被篡改后 UDP 还蒙在鼓里。所以校验和的覆盖范围是三段:`[伪首部 12B | UDP 头 8B | payload]`。

朴素做法是「伪首部算个 partial 校验和,L4 部分再接着累加」——但这要自己处理 fold(回卷进位)和 complement(取反)的衔接。这章换个干净法子:把三段**拼进一个连续缓冲区**,一把 `internet_checksum` 跑完(它自带 fold+complement),再把算出来的值嵌进 UDP 头的校验和字段。发送(`udp.cpp:90`):

```cpp
// 一个连续区 = [伪首部(12) | UDP 头(8) | payload(len)]
HeapBuf buf(12 + sizeof(UdpHeader) + len);
build_pseudo_header(buf.p, cfg->local, dst, udp_len);
UdpHeader h{src_port, dst_port, udp_len, /*checksum=*/0};
build_udp_header(h, buf.p + 12);
std::memcpy(buf.p + 12 + sizeof(UdpHeader), data, len);

uint16_t cs = internet_checksum(buf.p, 12 + sizeof(UdpHeader) + len);
if (cs == 0) cs = 0xFFFF;                 // RFC 768:算出 0 发 0xFFFF(0 表示「无校验和」)
// 把 cs 嵌进 UDP 头校验和字段,然后发 UDP 部分(跳过伪首部,伪首部不上线)
return ipv4.send(dev, dst, kIpProtoUdp, buf.p + 12, sizeof(UdpHeader) + len, stack);
```

(`udp.cpp:90`。)两个细节:计算校验和时,UDP 头里的校验和字段填 0(不然校验和会把自己算进去);伪首部只在校验和计算时用,**不上线**——真正发出去的是跳过伪首部的 UDP 部分。接收端(`udp.cpp:132` 的 `handle`)同构:重建 `[伪首部 | 收到的 UDP 段]` 跑 `verify_internet_checksum`。还有一个 RFC 768 的边角:发送方算出 0 要发 `0xFFFF`(因为 0 在 UDP 里是「我没算校验和」的意思),接收方看到 0 就跳过验证。

> 那个 `HeapBuf`(`udp.cpp:40`)不是随手用的——UDP 包能到 ~1.5 KB,这种大缓冲待在内核栈上会爆(内核栈才 16 KB,上一章 path 那个 4 KB 的坑同源)。所以放堆上,RAII 析构释放,跟 `ipv4.cpp` 里 IPv4 发包用的 HeapBuf 一个套路。

**端口多路复用。** `UdpModule` 自己也持一张小表(`kMaxUdpPorts = 16`,`udp.hpp:77`),`bind(port, listener)` 登记哪个端口归谁听,收到包按目的端口分派:

```cpp
virtual void on_udp(const Ipv4Header& ip, uint16_t src_port, FrameView payload) = 0;
```

(`udp.hpp:72`。)注意 `on_udp` 的 payload 是**借用**的——只在 listener 处理期间有效,listener 要留存得自己拷走。16 个槽是协议层够用;等做 socket 层(下一章),再按需扩到几百。这套 `bind`/`unbind` + listener 回调,就是将来 `socket()`/`recvfrom()` 底下的地基。

## 底子优先:loopback 先证明,再上 e1000

协议层做好了,怎么验?沿用上一章那套「底子优先」——先在 loopback 上确定性地跑一个 round-trip,再上真 e1000。

**loopback round-trip(`test_udp_loopback`)。** 镜像上一章的 `test_ping_loopback`:一个软件 loopback 设备,注册 UDP 进 IPv4 的 L4 表,bind 一个内核侧的 capture listener,`udp.send` 发 6 字节到 `127.0.0.1:7777`,**单次 `stack.poll()` 跑完整往返**——send → IPv4 → L4 表 → UdpModule → listener。然后断言 listener 被调了、源端口对、长度对、payload 对。

为什么坚持单次 poll 跑完?因为 loopback 没有 SLIRP 的时序,发即入队、下轮 poll 派发,一次 poll 必跑完往返——这是「确定性」的来源。它躲开了 e1000 RX 那个「主循环只在 trap 时跑」的时序坑(上一章讲过)。而且它走的是和生产**一模一样**的全链路:NetStack → Ipv4Module → L4 表 → UdpModule,伪首部校验和经真实 round-trip 存活(host 单测已证一遍,内核态再证一遍)。日志会打:`[net] loopback UDP: 6 bytes from port 1234 (round-trip in one poll)`。

**e1000 TX smoke(`test_udp_e1000_tx`)。** 再上真硬件:在真 e1000 + QEMU SLIRP 上发 UDP 到 `10.0.2.2`。握手照搬上一章 ping 的(ARP miss → request 出去 → poll sti/hlt → SLIRP 答 ARP → cache hit),断言 `arp.lookup(gateway)` 成功(SLIRP 答了 ARP,真 e1000 双向通)、`udp.send` 被 IPv4 接受(proto=17 包发出去了)。

> 一个诚实交代:这一步是 **TX-only**。QEMU 的 SLIRP 网关有 ICMP echo 服务(所以上一章 ping 能收回 reply),但**没有 UDP echo 服务**——发出去的 UDP 包 SLIRP 不会回。所以这里只验「UDP 包能蹭着 ping 已经验过的 e1000 L2/L3 TX 路径发出去」,不指望收 reply。这是有意的:不引入新的失败模式(要是指望 reply 又收不到,你就分不清是 UDP 协议错了还是 SLIRP 不回),TX 能发出去就够证明协议层接对了。真 UDP echo round-trip 要等有 socket API + 真对端。

## 验证

四层验证,绕开「SLIRP 不回 UDP」这个限制。

**第一层:host 单测,协议核心。** `test/unit/test_net_udp.cpp` 九个 case:UDP 头 round-trip、TX 线序(proto=17 + 端口 + 长度对)、send→IPv4 表→handle→listener 完整 round-trip(用 no-L2 mock 把 send 捕获的 IP 包直接喂回,无 QEMU/SLIRP)、校验和损坏丢弃、`checksum=0` 接受、无 listener 丢弃、双端口 demux、unbind、重复 bind 拒。`./build/test/test_net_udp` 跑下来全绿。

**第二层:内核 loopback round-trip。** `test_udp_loopback`,上面讲过,两腿都打 `[net] loopback UDP: 6 bytes ... (round-trip in one poll)`。

**第三层:ICMP 走新表不回归。** 这是「还债」这步的双保险——ICMP 从硬编码迁进 L4 表,两条 ping 腿(`test_ping_loopback` + `test_ping_e1000`)必须还通。实测 `[net] loopback ping reply`、`[net] e1000 ping 10.0.2.2 reply` 都在,证明迁表没把 ping 走窄。

**第四层:e1000 UDP TX。** `test_udp_e1000_tx`,真 e1000 上 UDP 包发出去(SLIRP 不回,只验 TX)。

`run-kernel-test-all` 跑下来全绿(单核 + `-smp 2` 两腿)。
