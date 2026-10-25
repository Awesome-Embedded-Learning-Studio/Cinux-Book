---
title: "04 · 配置空间的活页"
description: "配置空间头六十四字节是固定家,后面的活页靠能力清单找:状态字一位说有没有,0x34 起一张单链表,id 与 next 各一字节,四十八步硬上限防环,指针低两位保留位清零再读。遍历与解码都吃注入的读函数,宿主长凳摆假总线,内核传真端口,测的是同一份代码不是副本。MSI 窄宽两式,MSI-X 表形与两张窗口,解码成词。"
chapter: 27
order: 4
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - pci
  - msi
  - testing
---

# 配置空间的活页

认门找盘的那一卷,咱们把 PCI 的配置空间当一本户口簿使:两个端口一问一答,总线上插着什么,一台一台地问出来。可那本簿子咱们只读了头几页。一台设备的配置空间二百五十六字节,开头的六十四字节是各家一样的固定家:身份、类别、六条基址窗,咱们找盘那年用的就是这几页。第六十四字节往后的都是活页,谁有什么本事就往里夹什么,在 PCI 的说法里这叫能力,清单的名字也就叫能力清单:一块可选的功能描述, MSI 这样的消息中断、电源管理这样的事,都以活页的形式夹在后面。今天咱们要请的客人指名要读这些活页,所以翻页的功课得补上。

翻页的路数朴素得近乎简陋。状态字住在配置空间偏移六的地方,咱们读的时候连着低半的命令字,按偏移四的那枚三十二位整字取,第二十位说的是一台设备有没有夹活页,没有的话就不必翻。有的话,0x34 那个字节装的是头一枚指针,指向第一张活页的页码。每张活页的开头两字节,一字节是自己的身份号,一字节是下一张的页码,零就是到头了。不过一张单链表,就这么简单。偏偏链表这东西,坏起来也有一套:设备疯了,或者咱们自己读错了,指针绕成了一个环,走的人就永远出不来。所以遍历带一道硬上限,四十八步还没走完就放弃了,答出的就是零。四十八不是拍的数,它的来路算得出来:配置空间二百五十六字节,头六十四是固定的家,剩下的只有一百九十二,每张活页的头按整字对齐各占一格,一格一步的走法,一百九十二除以四出来的正是四十八,走到第四十九步必然重访某个走过的地方,环就成了真环。还有一处规范留的小脾气:指针的低两位是保留位,设备上什么值都可能,咱们读之前一律清零,按包含它的那个字对齐的位置去取。三条防御摆在桌上了,每一条都不是猜的,单链表的世界里,环、野指针、保留位都是真会遇到的客人。

活页的读法,咱们把遍历写成这样一个函数:它不碰任何端口,谁递给它读的函数,它就用谁的眼睛看配置空间。读函数长什么样,签名是一行的事:拿着设备的座位词与字节偏移,答一个三十二位的字。座位词是新立的词汇:总线、槽位、功能三个号,配得上叫一台设备在配置空间里的座位。遍历的真身咱们摆出来:

```cpp
uint8_t FindCapability(PciConfigRead read, const void* context, PciSpot spot, uint8_t wanted) {
    if (!ConfigWord{read(context, spot, kRegStatus)}.has(kStatusHasCaps)) {
        return 0;
    }
    uint8_t offset = aligned_pointer(read(context, spot, kRegCapabilityPointer));
    for (unsigned step = 0; offset != 0 && step < kCapabilityWalkLimit; ++step) {
        const ConfigWord kHeader{read(context, spot, offset)};
        if (static_cast<uint8_t>(kHeader.extract(kCapId)) == wanted) {
            return offset;
        }
        offset = aligned_pointer(kHeader.extract(kCapNext));
    }
    return 0;
}
```

走一遍它的骨架:头一句看的是状态字上的活页位,没有活页的设备连链表的门都不用进。门进了,每一圈读一枚整字当活页的头,低八位是自己的身份号,高一段的八位是下一张的页码,对上想要的身份号就交差。前文铺的三条防御全在这十来行里落了地:步数被 kCapabilityWalkLimit 摁在四十八,自环的链表走满就撒手答零。每次换页的时候都过一遍 aligned_pointer,把低两位的保留位裁掉了再走,野一点的指针也按包含它的整字对齐去读。在内核里读函数的真身是一个函数指针,老老实实地走 0xCF8 与 0xCFC 两个端口。宿主的长凳上给它假身:一块内存数组,咱们往里摆字节,想造什么设备就造什么设备。真假两个身吃的是同一个遍历、同一份解码,宿主长凳上测到的就是真代码本身。一旦写了两份代码,一份跑测试的,一份跑机器的,两份的脾气迟早长得不一样,当年第一遍的工程线上,成批的解码测试正是靠这一手养活的。

遍历有了,咱们翻头一沓活页:消息中断的两种方言。消息中断本身的门道,咱们用白话说:从 IRQ 那年起,设备喊内核走的是拽线,一根引脚一条线的配法,线少而设备多,分拣的活还得中断的门房来干。消息中断反其道而行,设备不拽线了,它往一个内存地址写一个字,地址写的是往哪送,数据写的是说什么。地址落在哪、怎么解码,是下一章的活,这一章只管把设备的两张方言活页读成词。老的一种叫 MSI,拿的身份号是 0x05:它能开几路向量,地址是三十二位还是六十四位,每路有没有单独的屏蔽位,当前的地址与数据各是什么,解码出的就是一个事实包。解码的真身:

```cpp
bool DecodeMsi(PciConfigRead read, const void* context, PciSpot spot, MsiFacts& into) {
    const uint8_t kCap = FindCapability(read, context, spot, kMsiCapId);
    if (kCap == 0) {
        return false;
    }
    const ConfigWord kHead{read(context, spot, kCap)};
    into              = MsiFacts{};
    into.enabled      = kHead.extract(kMsiEnabled) != 0;
    into.wide_address = kHead.extract(kMsiWideAddress) != 0;
    into.maskable     = kHead.extract(kMsiMaskable) != 0;
    into.vector_cap   = static_cast<uint8_t>(kHead.extract(kMsiVectorCap));
    into.address      = read(context, spot, after_header(kCap, 4));
    if (into.wide_address) {
        into.address_high = read(context, spot, after_header(kCap, 8));
        into.data         = static_cast<uint16_t>(
            ConfigWord{read(context, spot, after_header(kCap, 12))}.extract(kMsiData));
        return true;
    }
    into.data = static_cast<uint16_t>(
        ConfigWord{read(context, spot, after_header(kCap, 8))}.extract(kMsiData));
    return true;
}
```

窄门就在 if 的分支上:宽地址的设备,数据字的位置往后挪一格,窄宽两式的区分,活页自己倒是会说话,咱们照着摆位就是,两条岔开的读法就是两式的全部差别。新的一种叫 MSI-X,拿的身份号是 0x11,词表换了:它不再把向量挤在一格里,而是开一张表,一格向量一个表项的格局。解码同样是吃注入读函数的纯读:

```cpp
bool DecodeMsix(PciConfigRead read, const void* context, PciSpot spot, MsixFacts& into) {
    const uint8_t kCap = FindCapability(read, context, spot, kMsixCapId);
    if (kCap == 0) {
        return false;
    }
    const ConfigWord kHead{read(context, spot, kCap)};
    const ConfigWord kTable{read(context, spot, after_header(kCap, 4))};
    const ConfigWord kPending{read(context, spot, after_header(kCap, 8))};
    into                = MsixFacts{};
    into.table_size     = static_cast<uint16_t>(kHead.extract(kMsixTableSize) + 1);
    into.enabled        = kHead.extract(kMsixEnabled) != 0;
    into.masked         = kHead.extract(kMsixMasked) != 0;
    into.table_bir      = static_cast<uint8_t>(kTable.extract(kWindowBar));
    into.table_offset   = static_cast<uint32_t>(kTable.extract(kWindowOffset));
    into.pending_bir    = static_cast<uint8_t>(kPending.extract(kWindowBar));
    into.pending_offset = static_cast<uint32_t>(kPending.extract(kWindowOffset));
    return true;
}
```

活页里说的是表有多大,表放在哪条基址窗的什么偏移,待处理位图又放在什么位置。待处理位图是给屏蔽期挂起的中断留的标记数组,哪一路的向量被挡在了屏蔽外面,它的标记位就立了起来,设备靠它把没送出去的话记了下来。表的大小是编码值加一,答零的字段意味着一格的表,它读不准的话,后面就全错了。两张表的位置解出来,咱们顺手用找盘那年备下的基址解码词,把窗基址加偏移算成现成的地址,六十四位的窗把高低两半配对读,这是老词的又一次出场。

解码的活安在哪个家,咱们也拍了板:遍历归 PCI 家族,它本来就是总线簿册的延伸。两种消息方言的解码另立门户,因为它们不是 PCI 一家的私产,往后哪台别的设备要武装消息中断,用的是同一沓词。解码还有一个不动声色的讲究:所有字段一律从三十二位的整字上取,不另立十六位的控制字。位词表在窄整数上有整数提升的暗雷。十六位的字一进表达式,就被提升成带符号的整型,位的宽度与符号跟着变了味,裁出来的域未必是想要的那几位。咱们绕开它,读的时候按整字读,裁的时候从整字上裁。

宿主的长凳上,这一沓新词配了十几个案子:走查的命中、无清单、无此号、自环四十八步止、保留位清零,解码的窄式宽式、表形、窗口落位,一路全数过了。路上还自摆了一个乌龙,咱们如实交代:头一版测试里,咱们把下一页的页码错摆进了控制字段的位上,一个 0x00005011 写成了 0x00050011,走查的案子当场就把这个假设备抓了出来。假总线也有被摆错的时候,摆错了,当场红给你看,不用等机器黑屏的那天。活页翻得动了,消息中断的两种方言也认得了,下一章咱们去铺消息的新路本身:消息送进来,总得有一张接得住的座位。
