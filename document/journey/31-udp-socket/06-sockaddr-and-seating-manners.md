---
title: "06 · sockaddr 的形与落座的礼数"
description: "sockaddr_in 十六个字节是客人进门就按的 ABI:头两字节族字说本机序,端口与地址在线上说大端,门里抄写的时候现翻译,族字不是二答 EAFNOSUPPORT,短过八字节答 EINVAL。bind 的礼数三条:地址要么零要么自家,不然 EADDRNOTAVAIL。端口递零换短命号码。重了 EADDRINUSE。connect 在数据报的世界里只是记名,一根线都不出门。没 bind 就 sendto,套接字自己悄悄落座。write 走的是 connect 记下的对家,没记名答 EDESTADDRREQ。getsockname 把空地址填成自家,getpeername 没记名答 ENOTCONN。旋钮一排:几个常见的 setsockopt 点头答零,生面孔答 ENOPROTOOPT。getsockopt 答得出类型字与八格乘一千四百七十二的收容量。顺手修过一笔:recvfrom 的出参写地址,写完的字数差点当了返回值。"
chapter: 31
order: 6
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - networking
  - sockets
  - sockaddr
  - abi
---

# sockaddr 的形与落座的礼数

地址是客人跟套接字说话的名帖,递来时都得按 sockaddr_in 的形:十六个字节,头两字节放的是族,接着两字节放的是端口,再四字节放的是地址,剩下的位置空着。形的讲究在字节序上:族字按本机序说,端口与地址按线上的大端说,同一个结构里说的是两套话。门里的抄写现翻译,咱们看原文:

```cpp
long long read_sockaddr(Word address, Word length, net::SocketAddress& out) {
    if (address == 0 || length < kSockaddrMinBytes) {
        return -kEinval;
    }
    unsigned char record[kSockaddrInBytes] = {};
    const Word    kBytes                   = length < kSockaddrInBytes ? length : kSockaddrInBytes;
    if (!uaccess::CopyFromUser(record, address, kBytes)) {
        return -kEfault;
    }
    uint16_t family = 0;
    base::CopyBytes(&family, record + kFamilyAt, sizeof(family));
    if (family != net::kAfInet) {
        return -kEafnosupport;
    }
    out.port    = base::ReadWord16Be(record, kPortAt);
    out.address = base::ReadWord32Be(record, kAddressAt);
    return 0;
}
```

抄的多少随客人递的长短,短过八个字节连族带端口都装不下的话,答的是 EINVAL。族字不是 AF_INET 的,咱们家没有那扇门,答的是 EAFNOSUPPORT。往回写是同一张形的反方向,外加的是这么一个讲究:客人另递一个字长变量进来,说我的地址格有多大,咱们按格子大小写,写完了把真实字数十六写回那个变量,出参的出参,交代的是两层,哪一层都是不能省的。这一卷里这里还顺手修过一笔:出参写地址的函数答过一声写了多少字,门上那一眼是没细看的,把这一声当成了整个调用的答数,recvfrom 的返回值就差一点从收到的字数变成了地址的字数。出参的答数与调用的答数是两句话,写门的时候得分清。

bind 的礼数三条,咱们一条一条过。递零的意思是不挑地址,所以地址要么递零,要么递自家的地址。递别人家的,答的是 EADDRNOTAVAIL,咱们家没有那个门牌。端口递的是零,端口那一章的短命号码接手,发一个没人认的。端口重了答 EADDRINUSE,表上验过的话原样翻译。三件事全都过了,套接字就在端口表落了座,从此这个端口的包归它。

connect 在流式的世界里是握手,在数据报的世界里只是记名:把对家的地址记在本子上,一根线都是不出门的,对面的机器根本不知道咱们记过它。记名的用处落在两处。头一处的用场在 sendto 上:sendto 递空地址的时候,门问套接字要记下的对家,答不上来的话就是 EDESTADDRREQ,目的地是必须的。另一处在 write 上:文件的那副脸不知道地址,write 进了 transmit,transmit 见了空地址就去翻记名本了。所以 connect 加 write 与 sendto 带地址,两条路走到的是同一个汇上,客人们随自己的喜好挑一条。没 bind 就发的时候,端口那一章埋的那一笔在这兑现:套接字自己悄悄落座,领的是短命号码,发完了座还在,回声是有处投递的,客人下一次 getsockname 问起来的话,答的就是这个悄悄领来的号码。

问地址的两扇门都诚实:getsockname 把空着的地址填成自家地址再答,端口答真实坐下的那个。getpeername 的记名本空着就答 ENOTCONN,没连过谁的话,对家是报不出来的。旋钮一排也挂上了:setsockopt 点头的几个都是客人爱拧的,SO_REUSEADDR、收发缓冲的大小、SO_BROADCAST,拧了答零,心里记下的没有,这一卷的环不做大小生意。IP 层的 TTL 是跳数上限,与组播的 TTL 也点这个头。生面孔答的是 ENOPROTOOPT,咱们不装懂。getsockopt 答得出两样实在的:SO_TYPE 报出生的类型字,SO_RCVBUF 报环的真实容量:一千四百七十二乘八算出来是一万一千七百七十六的数,环多大报的数就多大,咱们不吹牛。旋钮的实现在往后的卷里一件一件补,眼下咱们把门挂对,拧了有答数。
