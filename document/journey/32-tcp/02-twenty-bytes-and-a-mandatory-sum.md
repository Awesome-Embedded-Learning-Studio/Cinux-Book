---
title: "02 · 二十个字节的头,一道不许免的算术"
description: "TCP 的头二十个字节,十个字:源端口、目的端口、序列号、确认号、数据偏移加旗标、窗口、校验和、紧急指针。词书补条目,拒因三个名字:不足二十字节的短,偏移小于五字的谎,头长越过实到字节的越身。词书只判头自己跟自己合不合,这是校验与回环栈那一卷立的家法。校验和这边 TCP 摆足了架子:伪首部十二字节押着 IP 头的字段,连同整个段折拢,折不拢零死得计数,refused_checksum 专名在册。UDP 挂在 IPv4 底下随了线上零当没算的俗,TCP 没有这个俗,校验和必算,当年第一遍的笔记里就明写 checksum 为零是协议违规。发送侧算出零改发 0xFFFF,两头的数照样对得上。长凳两案:短头与谎头都专名拒,坏校验和的段死在自己那口计数器上。"
chapter: 32
order: 2
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - networking
  - tcp
  - checksum
  - parsing
---

# 二十个字节的头,一道不许免的算术

协议的骨架从它的头讲起,这是咱们走过几层的老路了。TCP 的头二十个字节,不长,咱们按线上的次序念:源端口两字节,目的端口两字节,序列号四字节,确认号四字节,第十二字节的高半是数据偏移,说的是头自己占几四个字节的字,第十三字节是旗标,窗口两字节,校验和两字节,紧急指针两字节。头之后跟的就是货,多长没有字段直说,拿实到的字节减去头长就是。十个字里,序列号与确认号是这一卷的主角,窗口有专门一章的戏,旗标五枚,名字咱们报在头里:FIN、SYN、RST、PSH、ACK,一枚一个比特,谁立着谁说话。

头一个补的是词书的条目。校验与回环栈那一卷立的家法还在:每个协议一张自己的字表,收到的包死在哪一句,得说得出名字。咱们把 TCP 的判法抄来看:

```cpp
base::Result<TcpView, TcpError> ParseTcp(const unsigned char* segment, size_t length) {
    if (length < kTcpHeaderBytes) {
        return TcpError::kSegmentTooShort;
    }
    const unsigned char kHeaderWords = base::bit::HighNibble(segment[12]);
    if (kHeaderWords < kTcpHeaderBytes / sizeof(uint32_t)) {
        return TcpError::kBadDataOffset;
    }
    const auto kHeaderBytes = static_cast<unsigned char>(kHeaderWords * sizeof(uint32_t));
    if (kHeaderBytes > length) {
        return TcpError::kHeaderPastBytes;
    }
```

三道判,三个名字。不足二十字节的,叫短,连最起码的头都摆不齐。偏移声明的字数小于五的,叫偏移撒谎,五字二十字节是头的下限,声称自己比这还短,那是话没说圆。头长越过实到字节的,叫头超身,身体没有头声称的那么长。词书只判头自己跟自己合不合,这是老分工:实到的字节够不够声明的头,搬运的底下一层可能垫了填充,垫没垫只有模块知道,咱们这一层不越权。模块的计数器两口接住这三拒:短的一口叫 refused_short,谎与越身合用一口叫 refused_offset,长凳上各有各的案子。

然后是校验和,这一道 TCP 摆足了架子。伪首部十二字节,老朋友了:源地址、目的地址、零、协议字六、段长,押的是 IP 头里的字段,为的是这段字节就算被错投了对象,收方也算得出来。收侧怎么判,咱们看模块进门那一段,写得明明白白:

```cpp
    unsigned char pseudo[12] = {};
    build_pseudo_header(pseudo, header.source, header.destination, static_cast<uint32_t>(length));
    base::InternetChecksum verifier;
    verifier.add(pseudo, sizeof(pseudo));
    verifier.add(bytes, length);
    if (verifier.value() != 0) {
        ++stats_.refused_checksum;
        return;
    }
```

一段伪首部,一段段本体,同一个累积器接着折,折出来的和不是零,这段就死在这儿,咱们连它死在哪一口都叫得出名字,refused_checksum 专名在册。反码和的算术还是校验与回环栈那一卷立的那套,一笔没改。

架子摆在哪儿,咱们停下来看一眼。UDP 那一卷记过一个俗:线上校验和为零,当没算,直接放行,那是 UDP 挂在 IPv4 底下时随的俗,UDP 的头允许不算这门。TCP 没有这个俗。当年第一遍立 wire 层的笔记里明写:TCP 的校验和必填,算出来是零就是协议违规,没有不算的口子。道理也不深:UDP 是散件的生意,一个包丢了大不了重发一个。TCP 做的是字节的连号生意,每个字节都占着序列号上的一个号,一个字坏了,后头的号全跟着错位。这桩生意的底册不许有一笔糊涂的,校验和就是门卫,门卫不准请假。发送侧还留了一个小特务:真算出零来,改发 0xFFFF,与 UDP 同款的处理,两头折出来的数照样平,收方不会把真算的零错当没算。

长凳上两案守着这一章,咱们一案一案喂。头一案喂一个十九字节的短头,再喂一个偏移声明四字的谎头,两口计数器各记各的,名字断言到位,出门的段一个都没有。另一案把一个头样样齐整的段递进来,单单把校验和的字改成一个折不拢零的数,段死在 refused_checksum 那一口上,而且死得干净:后头答门的逻辑一步都没走到,计数器里连一笔别人的动静都没有。
