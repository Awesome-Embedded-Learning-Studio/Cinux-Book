---
title: "05 · 家族与环"
description: "套接字立成一个家族:基类管全家共用的两样,类型字与身份号,身份号出自一次开机一份的原子计数器,无盘起家那一卷传下的旧例。六张虚的脸,收、发、落座、指名、报自家、报对家,每个成员自己填。UDP 成员的家当是端口表上一个座加一只收环:环深九格用八格,留一格空,满与空才分得开。每格存的是回信地址加载荷的抄本,不是指针。递送的回调在环锁里头一件事就是抄,因为泵递的帧住在泵的缓冲里,锁只护环不护缓冲,出了锁还指着它就是悬空。环满了新来的丢,计数不报错,散件丢了不补。收在锁里弹出抄走,给多少截多少,数据报没有下半截。阻塞的礼貌不住在套接字里,住在门上。"
chapter: 31
order: 5
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - networking
  - sockets
  - ring
  - concurrency
---

# 家族与环

门上的号位挂齐了,该看号位背后站着谁。套接字立成一个家族:一个基类,几个成员,眼下成员有二,数据报的与裸的,流式的坐下一卷。基类管的是全家共用的两样:类型字,出生的时候记下,sockaddr 的问话拿它答。身份号,出自一次开机一份的原子计数器,每个套接字出生领一个,stat 问起来答得诚实,无盘起家那一卷立下的旧例,套接字照用。余下的本事全是虚的脸:收一张,发一张,落座一张,指名一张,报自家一张,报对家一张,每个成员自己填自己的。身份号的写法咱们念出来,就一行:

```cpp
Socket::Socket(int socket_type)
    : type_(socket_type),
      identity_(g_socket_identities.fetch_add(1, std::memory_order_relaxed) + 1) {}
```

基类的另一个身份咱们也得数一数:它数着引用。描述符表里每添一个座,引用加一。dup 一回,又加一。最后一个座撤了,释放的钩子穿过基类指针删掉整个对象,所以析构是虚的:基类指针只认得脸,删的时候得按成员自己的个头删,这是面向对象里少有的几处虚析构真不可省的地方。

咱们把 UDP 成员的家当数一数,两样:端口表上的一个座,一只收环。座的事前一章看过了,环是这一章的正主。环的格子长这样:

```cpp
struct Waiting {
    SocketAddress from{};
    unsigned char bytes[kUdpSocketPayloadBytes] = {};
    unsigned long length                        = 0;
};
```

一格三样:谁发来的,载荷的抄本,多少字节。请注意中间那一样,是抄本,不是指针。递送的回调进门的头一件事,就是把借来的载荷抄进自己的格子,写法咱们摆在这儿:

```cpp
void UdpSocket::take(void* context, const UdpDelivery& delivery) {
    auto* const kSelf = static_cast<UdpSocket*>(context);
    const auto  kGate = kSelf->ring_gate_.guard();
    Waiting     room{};
    room.from = SocketAddress{.address = delivery.sender, .port = delivery.sender_port};
    room.length =
        delivery.length < kUdpSocketPayloadBytes ? delivery.length : kUdpSocketPayloadBytes;
    base::CopyBytes(room.bytes, delivery.bytes, room.length);
    if (!kSelf->waiting_.push(room)) {
        ++kSelf->dropped_;
    }
}
```

为什么非要抄,咱们把两边的处境摆开。递送的是泵:泵的线程摇一圈栈,栈把帧从自己的缓冲里递出来,递完这一圈,缓冲就要给下一个包使了。收的是主人的任务:它可能正睡在门外,可能正在别的活上,反正不在这把锁里。锁护的是环,护不了泵的缓冲。回调要是只存个指针就交差,出了锁那一瞬间,指针指着的字节随时易主,主人醒来收到的是别人家的包。所以抄必须在锁内做完:进了锁,字节落进自己的格子,环外的一切变迁与格子无关。这是死命令,裸套接字那边同款,例外没有。

环本身是九格的容器,用八格。咱们想想为什么留一格永远空着:为的是满与空的分家。头尾两个游标要是格格都占,满环与空环长得一模一样,都是头咬着尾。留一格空,尾追到头前一格就算满,一格之差,两种状态各是各的。环满了咱们怎么办?push 答回假,新来的丢,dropped_ 记一笔。数据报生来是散件,收不动了丢在门口,比把门口堵死强。丢了几笔,dropped() 随时可以问,数挂在明处。

收的脸是同一只环的另一头,也活在锁里:咱们弹出一格,抄进客人的缓冲,给多少截多少,截下来的余量就地丢弃,数据报没有下半截,这一次没收完的不会留到下一次。回信地址顺手填进客人递来的出参,recvfrom 能报从哪来,靠的就是这次顺手填写。

咱们把析构的两行也念一念:撤端口表上的座在前,给环锁销号在后。撤座拿着关中断的守护,保证撤的动作不会被切走一半。销号是把这把临时锁从册上注销,免得这块地址往后被别人复用时,旧锁的记录缠到新主人头上。末了还有一句分家的话要说清:套接字自己不懂阻塞。收与发都是同步的内核调用,收不着答一声 would-block,发不出去答一声为什么,扭头就回。等着不走的礼貌住在门上,是门拿着 would-block 一圈一圈地试,门里的戏再过两章就上演。两只套接字两只端口,各收各的环,谁也听不见谁家的动静:端口表头一个匹配的座得包,一座一个主。
