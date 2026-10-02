---
title: 03 · 把 outb 藏进词汇表
description: "kernel 树添了 driver 器官目录,串口住进四层楼:print 只管排版,console 当薄脸面,serial 藏起寄存器和初始化表,io 把裸 outb 关进 .cpp。端口写成了对子,判忙写成了等位,表就是能读的数据手册。"
chapter: 7
order: 3
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - serial
  - driver
---

# 把 outb 藏进词汇表

嗓子认过了,条子也读过了,现在咱们得给它们安排住处。咱们再看内核这棵树:上一卷撑着它的还是 arch 和 boot 两个目录,本卷添了第一个器官目录 `kernel/driver/`。咱们把串口驱动安在 `driver/serial.hpp` 和 `serial.cpp`,底下还垫了一层 `driver/base/io.hpp` 和 `io.cpp`——驱动们的公共地基,往后谁家驱动来了都使得上。跟它们头顶上的 console 和 print 摆在一起,整条输出的路成了四层楼,咱们从最底下那层看起来。

io 这层的立意,是给端口 IO 配一套像样的词汇。最原始的写法您在 MBR 年代就见过:一句内联汇编 `outb`,端口和值都是当裸参数传的。咱们写一句两句不碍事,可驱动要是整篇都这么写,一个月后您回来看,满眼的 `%0`、`%1` 和魔法数,谁也认不出哪一笔是发给谁的。所以 io 这层立起来的头一件家当,是一个小小的对子:

```cpp
struct PortWrite {
    uint16_t port;
    uint8_t  value;
};
```

一次普普通通的端口写,从此就是一份明晃晃的数据:往哪儿、写什么,咱们全写在里头,而不再是一条指令加两个数。数据有什么好?这样的数据是能进表的,也能被循环一口一口地吃掉,咱们还能拿它一眼一行地读。于是 OutB 备了两个身形,单笔的和整表的:

```cpp
void OutB(PortWrite write);

template <unsigned long Count>
void OutB(const PortWrite (&writes)[Count]) {
    for (unsigned long index = 0; index < Count; ++index) {
        OutB(writes[index]);
    }
}
```

咱们还备了读端口的 InB,和两个判忙的动词 `WaitBitsSet` 和 `WaitBitsClear`。前者是原地打转的性子,直到掩码里的位全数置位。后者的脾气反过来,等它们全数清了零。至于那几句真正的裸汇编,它们被关进了 io.cpp 最里头的匿名名字空间——管端口 IO 的裸汇编,全内核树只此一家了。别处的代码想绕过词汇直接摸 outb?对不起,咱们这儿可是门都没有。倒不是谁拦着您,是匿名的符号根本递不出去。

词汇立好了,serial 拿它办的头一件事,就是把咱们递过的七张条子写成了表:

```cpp
constexpr PortWrite kInitSequence[] = {
    {.port = kCom1Base + kRegIer, .value = 0x00},  {.port = kCom1Base + kRegLcr, .value = 0x80},
    {.port = kCom1Base + kRegData, .value = 0x01}, {.port = kCom1Base + kRegIer, .value = 0x00},
    {.port = kCom1Base + kRegLcr, .value = 0x03},  {.port = kCom1Base + kRegFcr, .value = 0xC7},
    {.port = kCom1Base + kRegMcr, .value = 0x0B}};

void SerialInit() {
    OutB(kInitSequence);
}
```

您拿表跟上一节的七张条子对一对,一行对一行地核,包您严丝合缝。这就是把写变成数据的好处:初始化序列成了一张能读的数据手册,哪一笔发给了哪个寄存器、值又是多少,咱们眼睛扫过去就是答案,不用钻进函数体里一条一条地数。哪天您翻数据手册核对,表和手册摆在一起对照着看就行了。SerialInit 缩成了一行,倒不是咱们偷懒,是活儿真的只剩这么一点了。

发送的那一头,判忙也换成了词汇的写法。上一节咱们引过 SerialPutChar,这回看它字面上的意思:在 Lsr 的第 5 位上,等的就是 THRE 置位。您要写裸位运算,`(InB(port) & 0x20) == 0` 也一样是能跑的。可读出来的意思冷冰冰:与上 0x20、看结果是不是零,跟咱们想说的等发送处腾空,中间隔了翻译。咱们把意图写在字面上,位到值的换算、与或非的形状,全交给词汇去办了。

serial 自己的家当,咱们也交代一下:寄存器的偏移、THRE 的值、连同整张初始化表,全住进了 serial.cpp,公共的脸面,咱们对外的名字只剩两个,一个管开工的 SerialInit,一个管发字的 SerialPutChar。私有一律进 .cpp——这句话在内核这棵树里是成立的,因为内核是一个纯而又纯的 64 位世界:一种模式、一条链接,符号藏在 .cpp 里没有任何跨世界的风险。

boot 那头为什么不能照搬这一套?您别看 boot 也是“一个目标”,它一张二进制里混着 16 位、32 位、64 位三种模式的代码,情况就完全两样了。咱们当时真试着让 boot 也链 .cpp 的实现,结果链跑到一半机器就没了声息。这段没声息的现场,咱们留到 console 分家的时候对。

往上两层就轻了。console 扮的是一层薄脸面:InitConsole 转手请的就是 SerialInit,PutChar 转手递的就是 SerialPutChar,PutString 一个字符一个字符地循环,再让 Halt 睡它的长觉——前三个名字全是转发的活儿,Halt 是自家写的睡死。print 的那一层更是一行都没改:格式化还是 base 的老引擎,Println 还是老样子的 Println。您回想前面卷里把格式化和输出解耦的那一刀,今天它兑现了,咱们换了整条嗓子,排版的那层一行没动。格式化的那一卷说过,真到接串口的那天,咱们还得在引擎那边为后端开一个口子。您猜怎么着,许下的口子没用上——换嗓子的活,被 console 这一层整个接走了:引擎排完了自己的字只管往外送,底下接的是谁,它是一向不管的。四层楼的分工立住了:print 不认识端口,console 连寄存器的边都不摸,serial 的地界里连一行裸汇编都找不着。往后输出再出什么花样,咱们改哪层,就是哪层的事。
