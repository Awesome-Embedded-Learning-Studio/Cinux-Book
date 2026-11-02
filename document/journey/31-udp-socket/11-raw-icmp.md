---
title: "11 · 不穿词书的直通道"
description: "ping 亲口说 ICMP,自己构回声请求自己解应答,UDP 那本词书它一页不用,咱们给它开 SOCK_RAW:类型字三,协议字必须是 ICMP,不然 EPROTONOSUPPORT。不是根用户,EPERM,人家内核的家里同款门槛。RawModule 十六个座,汇那边挂一个观察哨,判完头两次、放行分派之前,把完整数据报连头递给哨位,ping 的解析从 IP 头自己读起,头也是它要的货。递送在名册锁内抄走借来的帧,环满就丢,同款的环纪律。发送直接交给 IPv4 装信封,载荷上限一千四百八十,UDP 的词书一字不沾。判例一桩:回环上发一发声请求,裸套接字收着两份,请求与应答各三十二字节,第二十字节是类型字,一份八字一份零字。"
chapter: 31
order: 11
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - networking
  - raw-sockets
  - icmp
  - ping
---

# 不穿词书的直通道

侦察的清单上,SOCK_RAW 是 ping 的进门证:它不要 UDP 的端口,不要 UDP 的头,回声请求自己一个字节一个字节地构,应答自己一个字节一个字节地解,它要的是 IP 层往上的原始货。咱们给它开的门在 socket 的号位上:类型字递三,协议字必须递的是 IPPROTO_ICMP,递了别的答 EPROTONOSUPPORT,咱们只备了 ICMP 的货。另外咱们还立了一道检查,euid 不是零答 EPERM:裸套接字看得到别人家的包,眼力是只发给根的,人家内核的家里同款门槛,咱们照办。壳里的客人是以根身份跑的,ping 是进得来的。

收货的座叫 RawModule,十六个座配的是一把自旋锁,与端口表是表亲的关系。挂进世界的手法另有一条路:不占四层的座,在汇的那边挂观察哨。哨位开火的那一行,咱们看原文,位置是在 IPv4 判完了头两次之后、放行协议分派之前:

```cpp
if (observer_ != nullptr) {
    observer_(observer_context_, bytes, kEffective, kView.value());
}
```

递出去的是完整数据报,连 IP 的头都在内。咱们选这个形不是随手选的:人家内核的裸套接字交的就是连头的货,ping 的解析也正是从第一个字节读起,IP 头里自己就带了长度,ping 拿它定位后头的 ICMP。少给一个头的话,ping 当场就算错了位置。哨位是在判两次之后开火的,所以送进来的头是验过校验的。在分派之前开火的话,走 UDP 座的包,哨位也是看得见的。眼力是给了哨位的,收不收是另说的,筛子是模块自己家里的:

```cpp
void RawModule::handle(const unsigned char* bytes, size_t length, const Ipv4View& header) {
    if (header.protocol != kIpv4ProtocolIcmp || length > kMaxDatagramBytes) {
        return;
    }
    const auto kGuard = gate_.guard();
    for (const auto& listener : listeners_) {
        if (listener.context != nullptr) {
            listener.receive(listener.context, bytes, length, header);
        }
    }
}
```

协议字不是 ICMP 的,咱们就过。长度过了一千五的,咱们也过。剩下的,咱们看名册锁内逐座递:递进去的回调是各家的 take,进门拿自家的环锁,头一件事照旧是抄进自家的格子,借来的帧住在汇的缓冲里,出了锁就易主,这个死命令是家族与环那一章立过的,裸套接字是没有豁免的。名册锁内嵌的是环锁,次序是单向的:名册在外,环是在内的,反过来是没有的,叶子锁的册不乱。环也同款:九格的容器用八格,满格丢的是新客,计数是不吵的。落了座想挑剔一点也可以:bind 记下的本地地址与包的目的地对不上,是不收的。connect 记下的对家与包的来处对不上,也是不收的,套接字的世界里这叫过滤,裸套接字照样是使得的。

发送的路反而更短,咱们一眼就能望到底:裸套接字的 transmit 直接把载荷交给 IPv4 的 send,协议字写死的是 ICMP,信封是由 IPv4 来装的,UDP 的词书从进门到出门一个字没沾。载荷的上限一千四百八十,一千五百减去 IP 头的二十,信封要占的地方留足。门上的 sendto 也认这个上限,超了答的是 EMSGSIZE,这是句老翻译了。

判例一桩讲的是回环上的两份收货。壳里发一声回声请求上了回环,汇把包原样绕了回来,进 IPv4 的收路:判两次过了,哨位开了火,裸套接字收着第一份——自己发出去的请求。ICMP 的座接着也判过了,应答一构好又上了回环,哨位又开了一回火,第二份是对面的回声。两份的长短是各三十二字节,咱们数过:二十的 IP 头加十二的 ICMP,第二十字节放的是类型字,请求里放的是八字,应答里放的是零字。头一回在长凳上跑回环案的时候,咱们连出参的字长都验了:递进去一个四字节的字长变量,收完它老老实实变成了十六,紧跟在它后头的金丝雀原封没动。阻塞的礼貌也同款:非阻塞的空收答 EAGAIN,信号来了答 EINTR,门是只有一扇的,换的衣服不换老师。料备齐了,大戏就开锣了。
