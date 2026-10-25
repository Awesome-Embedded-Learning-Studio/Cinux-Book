---
title: "02 · 三本册子,一座名册"
description: "字符是字节流要占文件座,块是扇区归块层,网络是帧连编号都没有。既有件各归各位:纯计算四件与 console 归字符册,盘只登记不改行为,回环设备当网络册首住户。概念六方法配擦除表,回环发即入队读才出队。观测座把三节名册照给 /proc,实弹敲 busybox 那一行 cat。"
chapter: 27
order: 2
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - device-model
  - netdev
  - procfs
---

# 三本册子,一座名册

门房立好了,册子怎么分,咱们照设备的营生分。生产内核把设备分作了三册,咱们照办,而且分的理由跟人家是同一个:字符册收的是字节流,一个名字后面淌着读不完写不尽的字,它要的是设备文件世界里的一个真座位。块册收的是扇区,它的主顾是块层与文件系统,人家取货按的是块,不走文件的门。网络册收的是帧,它的主顾是协议栈,帧里装的是包,连个设备编号都免了。三本的差别落到登记那一口气里:字符设备必须带脸,登记与落座办的是同一件事。块设备与网络设备不碰设备文件系统的边,登记的事归登记,那边的格子一个不占。册类决定的是设备跟文件世界有没有那层关系。

轮到既有件归位了,咱们挨个请。null 与 zero 两个纯计算的,console 这样的终端,还有 ptmx 与它铸出的对子,全部归字符册的名下,它们的脸是现成的,如今入座的路上多了一笔名册的记录。盘那边归的是块册,咱们要特别说清:只登记,行为上的事一概不碰。它今天在块层当差的一切,队列、缓存、扇区读写的老三样,咱们一个字都不动,登记的意义是让名册知道机器上有一台叫 sata0、主号八次号零的块设备,从 PCI 总线来的,管它的是 AHCI 驱动。名册是事实的登记处,行为的开关不在它手里,咱们不越界。

网络册是全新的脸面,咱们多花几段。起头的是概念:一台网络设备要会六件事。它有没有硬件地址,地址是什么,线上带的是不是以太网头,最大帧的上限是多少,收一帧的本事,发一包三层的载荷。六件事咱们写成 concept,在编译期替咱们把关:

```cpp
template <typename Dev>
concept NetworkDevice = requires(Dev& device, const MacAddress& destination, EtherType type,
                                 const std::uint8_t* payload, unsigned long length, RxFrame& into) {
    { device.has_mac() } -> std::same_as<bool>;
    { device.mac() } -> std::same_as<MacAddress>;
    { device.has_ethernet_header() } -> std::same_as<bool>;
    { device.max_frame() } -> std::same_as<unsigned long>;
    { device.poll_frame(into) } -> std::same_as<bool>;
    { device.send_l3(destination, type, payload, length) } -> std::same_as<bool>;
};
```

咱们看 requires 列表:那六件事一件一行地躺着,谁也冒充不了。承重的地方在签名上:方法全写足了类型,收一帧递出去的是 RxFrame,发一包收进来的是三层的载荷与目的地址,以太网头的事压根不出现在脸上,加不加头是设备自己的事。任何一个普通的类,只要把六个方法都写齐了,concept 就认下了它,继承省了,手写的函数表也省了,块设备那一类用的正是同一对形状,咱们隔着一个目录把这套手法搬了过来。

概念之上咱们还要再放一张擦除的表:六个普通函数指针,从概念检查过的类型一次性生成:

```cpp
struct NetOps {
    /// Whether the device owns a hardware address.
    bool (*has_mac)(void* self);
    /// The device's hardware address, zeros when it keeps none.
    MacAddress (*mac)(void* self);
    /// Whether the device's wire carries an Ethernet header.
    bool (*has_ethernet_header)(void* self);
    /// The largest frame the device moves.
    unsigned long (*max_frame)(void* self);
    /// Hands one received frame out, or answers false.
    bool (*poll_frame)(void* self, RxFrame& into);
    /// Takes one layer-three payload for the wire, or refuses it.
    bool (*send_l3)(void* self, const MacAddress& destination, EtherType type,
                    const std::uint8_t* payload, unsigned long length);
};

template <NetworkDevice Dev>
[[nodiscard]] constexpr NetOps EraseNetworkDevice() {
    return {
        [](void* self) { return static_cast<Dev*>(self)->has_mac(); },
        [](void* self) { return static_cast<Dev*>(self)->mac(); },
        [](void* self) { return static_cast<Dev*>(self)->has_ethernet_header(); },
        [](void* self) { return static_cast<Dev*>(self)->max_frame(); },
        [](void* self, RxFrame& into) { return static_cast<Dev*>(self)->poll_frame(into); },
        [](void* self, const MacAddress& destination, EtherType type, const std::uint8_t* payload,
           unsigned long length) {
            return static_cast<Dev*>(self)->send_l3(destination, type, payload, length);
        },
    };
}
```

咱们靠 EraseNetworkDevice 这个模板生成它:六个不捕获的 lambda 把调用一个个转发给真实类型,每个都落得成普通的函数指针,设备自己的指针当第一个参数,在调用的那一刻递进去。新设备添进来不用写任何的样板代码,类型转换只发生在生成它的那一步。消费面拿到的是一枚端口,收留设备的动作是这么几行:

```cpp
    template <NetworkDevice Dev>
    void adopt(Dev& device) {
        static constexpr NetOps kErased = EraseNetworkDevice<Dev>();
        ops_                            = &kErased;
        self_                           = &device;
    }
```

咱们再往里看:擦除的活就在 adopt 这一步一次做完,端口里存的就是一张表与一个设备指针。至于端口背后坐着的是哪台设备,持端口的人是永远不知道的,空的端口连问题都静默地拒答。今天坐着的是回环,后面哪一卷换了真网卡,持端口的人一个字都不用改。

回环设备住进来当了首住户,咱们看它的脾气。它没有硬件地址,线上也省了以太网头,两个问题它都答了个没有。发一包的时候,它把载荷原样压进一条四帧的队列。收一帧的活来了,队列里弹出了一帧。这里的意思是发与收之间隔着一道排队:发方一口气发五包,第五包怎么也进不去,队列答的是一声满,读的人来晚了,按入队的次序一帧一帧领。超长的包答不了,满队的发答不成,失败了也不装成功。为什么头一个请它进门,理由往后的网络那几卷会慢慢讲开,咱们把话搁在这:协议栈要在一个确定性的设备上证明自己,图的是发即入队、读才出队、次序不乱,这样的证人全机只有它。宿主长凳上它的七个案子,从逐字节的回放到擦除表的双设备,一路全数过了。

名册立了,总得让大伙看得见。观测座咱们开在 /proc 下面,它的名字就叫 devices,照的还是假文件与真终端那一卷立的路数:事实在读取的那一刻现场生成,活着的才在册。读取的活全部委托给名册:名册按序号递出每台设备的事实,观测座管的是排版,三本册子各占一节的版面,一行坐的是一台设备,名字带上了主次号,网络册的名字不带号。实弹咱们现在就打,在提示符下敲一把 busybox 的 cat,串口原文咱们逐字抄来:

```text
/ # busybox cat /proc/devices
[kern] syscall 40 unserved
character:
  null 1:3
  zero 1:5
  console 5:1
  ptmx 5:2
  usbkbd 10:0
block:
  sata0 8:0
network:
  lo
/ # 
```

三节名册坐了七行住户,与名册里的记录一台不差。细心的你会看见中间那行 [kern] 的报话:四十号系统调用没人接。四十号就是 sendfile 的号,这不算什么病。busybox 的 cat 头一步试着走 sendfile,内核答了未实现,它老老实实退回 read 与 write 的老路,输出的字一个不差。客人试探的是新门,旧门倒是还在。表里还有一位叫 usbkbd 的客人,字符册的第五行,客人是本卷后段才进门的,咱们暂且把它挂在表里,等它进门那一章再讲它的来历。设备文件系统那边的容量也顺手从八座添到十六座,散件归了册之后,座位是要跟着名册一起宽裕的。

三本册子开张了,门房的第一桩事办完了。可假文件与真终端那一卷收官时挂下的疑点还躺在案上,说的是 script 的会话收不回尾。那是伪终端欠的活,不关门房的事,咱们下一章单独收它。
