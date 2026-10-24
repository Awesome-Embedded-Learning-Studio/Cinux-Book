---
title: "09 · 一轮 poll 一个来回"
description: "长凳接线:一台回环、一张 NetPort、一台栈、两个件、两个汇,两个件都活了才接线,构造期谁也不欠谁。回环纯内存零延迟,时序攥在自己手里,当年在回环上跑 ping,躲的就是用户网络异步投递的回包。第一案一轮 poll 跑完 ping 完整来回,字节跟着走一遍全链,记分牌逐项对上。第二案预算缰绳:三只请求只给两帧,答二见零,再来一轮全清。"
chapter: 29
order: 9
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - networking
  - loopback
  - ping
---

# 一轮 poll 一个来回

链立齐了,咱们在长凳上接线。
接线的原样就在长凳的案头,咱们裁出关键的一段:

```cpp
struct Wiring {
    cinux::driver::net::NetPort port;
    cinux::net::NetStack        stack;
    cinux::net::Ipv4Module      ipv4;
    cinux::net::IcmpModule      icmp;

    Wiring() {
        cinux::driver::net::Loopback::self().init();
        port.adopt(cinux::driver::net::Loopback::self());
        stack.init();
        stack.attach_port(port);
        const cinux::net::DatagramSink kToWire = {
            .context = &cinux::driver::net::Loopback::self(),
            .send    = send_to_loopback,
        };
        ipv4.init(kToWire, cinux::net::kLoopbackAddress);
        const cinux::net::DatagramSink kViaIpv4 = {.context = &ipv4, .send = send_via_ipv4};
        icmp.init(kViaIpv4);
        cinux::net::L4Seat icmp_seat;
        icmp_seat.adopt(icmp);
        ipv4.seat_protocol(cinux::net::kIpv4ProtocolIcmp, icmp_seat);
        cinux::net::ProtocolSeat ipv4_seat;
        ipv4_seat.adopt(ipv4);
        stack.seat_protocol(cinux::net::kEtherTypeIpv4, ipv4_seat);
    }
};
```

接线的样子照实描:一台回环,天生的单例,init 清了队列。一张 NetPort 的 adopt 把它包成口,栈的 attach 再把口挂进端口表。头一个 init 的是 IPv4,汇瞄着回环的 send_l3,自家地址写 127.0.0.1。随后 init 的是 ICMP,汇瞄着 IPv4 的 send。然后落座:ICMP 坐进 IPv4 门下协议一号的座,IPv4 坐进栈的类型字 0x0800。顺序上是有讲究的,两个件都活了才接线,构造期谁也不欠谁什么,前头第七章立的话在这里兑现。接线的是一个普通的本地结构,口、栈、两个件全是它的成员,咱们在测试函数里自己造一台,测完了就扔。栈不是单例的好处,这里见着了:一台长凳一台栈,谁也不串谁的门。

回环为什么好用,send_l3 的做法就是答案:字节原样塞进队列,没有地址没有头,到 poll 的时候才出队,走的是纯内存,给的是零延迟,时序攥在咱们自己手里。当年第一遍是从回环上跑 ping 起步的,躲的就是模拟器里那个用户网络异步投递的回包,你不知道它哪一拍才到。重走的这一遍理由原样成立,收官的 ping 就跑在回环身上。

咱们看第一案:ping 的完整来回。咱们造一只十二字节的回声请求,标识写了 0x1234,序号写了一,交给 IPv4 的 send。后面的事一环扣一环,咱们跟着字节走一遍。IPv4 构好二十字节的头,连头带载荷的三十二个字节整只交给了汇,汇调回环的 send_l3,字节进了队列。poll 开动了,预算给了十六。栈从口上拿到了帧,口是没有以太头的,字节直送 IPv4 的座。IPv4 判了两次,全过了,载荷连头交给了 ICMP。ICMP 见到的是请求,当场构了应答,交给自己的汇,汇调 IPv4 的 send,又构了头,又进了队列。这一趟 poll 的预算还剩着,同一趟排干捡到了第二帧,再判再派了一回,ICMP 这回见的是应答,记了分。poll 一趟跑完了,记分牌上:答过的请求一,见过的应答一,最后的标识 0x1234,序号还是原来的一。IPv4 那边的 delivered 二、sent 二,栈那边的 delivered 二。一个来回的事,一轮 poll 就办完了,从这端到那端的通路,没有一个字节出了内存。

咱们接着看第二案:缰绳。咱们连发三只请求,然后只给两帧的预算。这一轮跑完了,送出去了两帧,答了两声,见着的应答一声都没有,队列里躺着的是应答,预算没了。再来了一轮,给足了十六,三声全答了,三声也全见了,什么也不欠。预算是循环的皮带,这个案把皮带勒给大家看:它不是性能的限制,是保命的,处理中发、发了又处理的环,没了皮带勒着,就成了死循环。

两案在长凳上过了。整条的链,从校验和的算术到 ICMP 的记分,没有一个字节依赖机器的世界,它依赖的只有递进来的缓冲与外面接好的汇。咱们跑的是同一份代码,长凳上过了一遍,待会儿机内的世界再来一遍。这就是这一部敢把整个协议栈立在开发机上的底气。
