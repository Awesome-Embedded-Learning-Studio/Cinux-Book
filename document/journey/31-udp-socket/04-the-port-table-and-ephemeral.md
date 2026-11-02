---
title: "04 · 端口那张表与短命号码"
description: "UDP 的端口表是栈那边词汇的扩座:八个座,一把自旋锁,bind 验重复验满座才落笔,unbind 拿末座补缺,次序不是表的本分。短命号码是给没报名的客人备的:区间三万二千七百六十八到六万零九百九十九,跟人家内核默认的区间一个数,回回从段首扫起,头一个没主的号就是它,全段有主才答零。发路的活一气写完:一千四百七十二的载荷上限,栈上一千五百字的缓冲,四个头字段全用大端写词,伪首部连同数据报折出校验和,算出零发全一的特例照发,信封上协议字十七,交给汇。收侧查表也在同一把锁里,头一个匹配的座得包,没主的照旧计数。"
chapter: 31
order: 4
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - networking
  - udp
  - ports
  - ephemeral
---

# 端口那张表与短命号码

包过了三重门,下一句问的是:这个端口谁认。认端口靠的是一张表,表的词汇校验与回环栈那一卷早就立过:端口表,类型字的座,四层的座。UDP 这边把表开在了模块自己家里,开的是八个座,配的是一把自旋锁,一个座位记的是三个字:端口号、听客的上下文、递送的回调。落座与退座的两段,咱们对着原文看:

```cpp
bool UdpModule::bind(uint16_t port, void* context, UdpDeliver deliver) {
    const auto kPorts = ports_gate_.guard();
    if (deliver == nullptr) {
        return false;
    }
    if (port_count_ == kUdpPortSeats) {
        return false;
    }
    for (unsigned long index = 0; index < port_count_; ++index) {
        if (ports_[index].port == port) {
            return false;
        }
    }
    ports_[port_count_] = {.port = port, .context = context, .deliver = deliver};
    ++port_count_;
    return true;
}

bool UdpModule::unbind(uint16_t port) {
    const auto kPorts = ports_gate_.guard();
    for (unsigned long index = 0; index < port_count_; ++index) {
        if (ports_[index].port == port) {
            ports_[index] = ports_[port_count_ - 1];
            --port_count_;
            return true;
        }
    }
    return false;
}
```

落座验的是两样:满不满、重不重。八个座都坐了人,答的是坐不下。端口号已经有人认了,答的是名花有主。退座的写法值得咱们看一眼:拿末座顶掉要走的那一座,表就短了一格,其余的座不用搬。表从来是不说次序的,查表本来就是要从头扫到尾的,谁坐在第一座都是没有分别的,搬移整队反而是白花的力气。门那边把这两句答话翻成了 errno,坐不下与重了答的都是 EADDRINUSE,是地址在用的意思,客人一听就懂了。

短命号码是咱们备的另一桩体贴。客人们常常是不挑端口的:bind 递个零进来,意思是随便给我一个。更有连 bind 都省的,sendto 就直接出门了。这些客人也得有个回信地址,不然对面的回声无处投递。咱们备的区间是三万二千七百六十八到六万零九百九十九,人家内核默认的短命区间就是这个段,咱们对齐它,客人拿咱们当人家使的时候,行为是对得上的。发的算法是老实的扫段:回回从段首三万二千七百六十八扫起,头一个没人认的号码就是它。扫遍整段都是有主的,答的就是零,门把这句翻译成了 EADDRINUSE。这一步在咱们的栈上踩不到:段里两万八千多个号,端口表只有八个座的位置,真要满了,也是 bind 的门口早答过坐不下,就轮不到扫段来报了。答零的口径挂在码上,是把理论上的死角如实报出来、免得它闷着。

发路的活,一头连的是端口表,一头连的是汇,咱们接着看原文:

```cpp
bool UdpModule::send(uint32_t destination, uint16_t source_port, uint16_t destination_port,
                     const unsigned char* payload, size_t length) {
    if (length > kMaxDatagramBytes - kMinIpv4HeaderBytes - kUdpHeaderBytes) {
        return false;
    }
    unsigned char outgoing[kMaxDatagramBytes]{};
    const auto    kDatagramBytes = static_cast<uint16_t>(kUdpHeaderBytes + length);
    base::WriteWord16Be(outgoing, 0, source_port);
    base::WriteWord16Be(outgoing, 2, destination_port);
    base::WriteWord16Be(outgoing, 4, kDatagramBytes);
    base::WriteWord16Be(outgoing, 6, 0);
    for (size_t index = 0; index < length; ++index) {
        outgoing[kUdpHeaderBytes + index] = payload[index];
    }

    unsigned char pseudo[kPseudoHeaderBytes] = {};
    build_pseudo_header(pseudo, own_address_, destination, kDatagramBytes);
    base::InternetChecksum builder;
    builder.add(pseudo, sizeof(pseudo));
    builder.add(outgoing, kDatagramBytes);
    uint16_t checksum = builder.value();
    if (checksum == 0) {
        checksum = 0xFFFF;
    }
    base::WriteWord16Be(outgoing, 6, checksum);

    if (!sink_.assigned()) {
        return false;
    }
    const OutboundDatagram kDatagram = {
        .destination = destination,
        .protocol    = kIpv4ProtocolUdp,
        .payload     = outgoing,
        .length      = kDatagramBytes,
    };
    sink_.send(sink_.context, kDatagram);
    ++stats_.sent;
    return true;
}
```

头一行就是天花板:一千五百的最大数据报,减去 IP 头的二十,减去 UDP 头的八,剩下的是一千四百七十二,载荷过了这个数,连发都不发了。send 的缓冲就开在栈上,开的是一千五百字,讲究在并发上:缓冲要是开成模块家里的成员,全模块合用的就这么一份,两颗核同时各发各的包,后落笔的就把人家写了一半的盖掉,另一头折校验和的时候,折的已经是别人家的字节了。调用栈上的缓冲正好躲开了这层共享:一次调用就自带了一份,谁也盖不着谁的包。您要是心疼这点栈,动了把缓冲提成成员的念头,咱们劝您把它按住:双核的机器上,写一半被盖的案子当场就回来。管理堆的省心只能算白捡的,内核里这点栈是花得起的。四个头的字段全用大端写词落笔,校验和那一栏起手写的是零,因为零也进了折叠的数。校验和是拿伪首部连同整个数据报折出来的,算出了零就改发 0xFFFF,收发两头的数上一章都对了,这里就不再重讲了。算完之后信封一封:目的地址、协议字十七、数据报本体,一起交给了汇。汇没接线的时候,assigned 是空的,如实答的是发不出去,一声不吭地吞是咱们不干的事。

收侧查表咱们接着看,就在 handle 的后一半,用的还是同一把锁:从头扫起,头一个端口号对上的座得包,递送的是载荷连同回信地址,计数器记的是一笔 delivered,扫到了底也没人认,记的是一笔 unclaimed。查表是在锁内的,递送也是在锁内的,这里埋一句伏笔:递送的回调进门还要再拿一把锁,自己的环锁,拿了锁头一件事是把借来的载荷抄走。为什么非要抄,下一章见到环就明白了。
