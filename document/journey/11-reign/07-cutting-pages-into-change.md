---
title: 07 · 把页切成零钱
description: "页表管到 4KiB 就到底了,可内核常要的是几十字节。Heap 类只懂算术:32 字节头记身家,8 字节尾巴记跨度,空闲块按地址排队,first-fit 挑块、够大就分裂、还回来两边合并。三个真 bug 全让长凳上的 host 测试当场按住。"
chapter: 11
order: 7
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - memory
  - heap
---

# 把页切成零钱

咱们把一笔数算清,堆的动机就全在了。一个进程对象、一条链表节点、一张小小的描述符,几十个字节的事。可眼下内存子系统的最小面额,才到 4KiB 一页——为一个 64 字节的结构体占一整页,六十四倍的零头利钱。更麻烦的是管理:一页记一个主,主里住着的是几十件小东西,哪一件是谁的、哪一件谁在用,靠人一笔一笔地记,是根本记不清的。所以页之上还得有一层铺子,把整页换成了零钱。本节咱们开铺子,而且从起手就把它劈成两半:算术的半边和页的半边。铺子的挑块、核数、合并全是纯算术,给它一段现成的内存就能开张。往铺子里进货的事(页从哪来、怎么映射进堆窗口)归下一节。劈开的理由很实在:算术半边不碰页表,咱们在长凳上就能真测,连开机这一步都省了。

铺子的家底刻在每一块上。咱们拿一段 span 起手,把整段当成了一个空闲块,往后的日子都在里面切。块长什么模样,heap.cpp 匿名空间里开头的几行就是全部:

```cpp
constexpr unsigned long kHeaderBytes  = 32;
constexpr unsigned long kFooterBytes  = 8;
constexpr unsigned long kPayloadAlign = 16;
constexpr unsigned long kMinPayload   = 16;

struct Header {
    unsigned long payload;
    unsigned long state;
    unsigned long next;
    unsigned long spare;
};
static_assert(sizeof(Header) == kHeaderBytes);
```

头的四个 8 字节各司其职:payload 记的是负载多大,state 记的是闲忙,next 是空闲链上的后继,spare 留着做对齐的余量。结构体底下紧跟的 static_assert 把“32 字节头”焊在了它自己身上,谁要是多塞了一个字段,编译期当场就说了话。每一块的后头还拖着一个 8 字节的尾巴,里面写的只有一件事:本块的跨度,咱们把头、负载、尾加起来一共多少字节。写尾读尾的就是一对小帮手:

```cpp
unsigned long span_bytes(unsigned long payload) {
    return kHeaderBytes + payload + kFooterBytes;
}

void write_footer(unsigned long block, unsigned long payload) {
    *cinux::base::PtrAt<unsigned long>(block + kHeaderBytes + payload) = span_bytes(payload);
}

unsigned long read_prev_span(unsigned long block) {
    return *cinux::base::PtrAt<unsigned long>(block - kFooterBytes);
}
```

write_footer 把跨度写进了块尾,read_prev_span 反着用:本块头前那 8 字节躺着的,正是前邻的尾巴,读出来的就是前邻的跨度。头是顺走用的,尾巴是回走用的,往后看您就明白,合并邻居的时候,尾巴就是咱们的回头路。

空闲的组织是一条按地址排队的链。为什么要按地址排?这里有个取舍咱们摆开说:当年第一遍的空闲块是乱序链,合并靠的是全链扫描、多趟反复,块一多就按平方级地慢。这一遍链打起手就按地址排好,合并找邻居的当口,一块的前邻后邻在链上就是左右邻座,顺藤摸过去就是了。挑块的策略是 first-fit:从链头往下找,头一个装得下请求的块就成交。这个策略的脾气您得知道:低地址的块永远头一个被挑,掏零钱掏的永远是链头,日子久了小碎片攒在低处。跟它对照的是 best-fit——全链扫完挑最合身的,碎片是省了,可每一笔都要付全链扫描的钱。内核的堆不追求极致,咱们选了便宜的 first-fit,碎片的问题交给合并去兜底。

成交之后的第二幕是分裂。挑中的块要是比请求大出不少,大的那部分不该跟着陪葬——余下的头尾开销加最小负载要是还装得下一个最小块,就从中间的位置切一刀,前半成交了,后半回到了链上。切不出来就整块给你,零头算买家的。最小负载咱们定 16 字节,对齐也定 16:所有负载都向上取整到 16 的倍数,出来的指针天然对齐,买家里最挑剔的 SIMD 也不挑礼。

还钱是最有戏的一笔。咱们收下一笔 free,要过的头一道工序就是验身,Heap::free 开头的原文是这么两道闸:

```cpp
    unsigned long const kBlock = reinterpret_cast<unsigned long>(block) - kHeaderBytes;
    if (kBlock < base_ || kBlock >= top_) {
        return false;
    }
    Header* const kHeader = header_at(kBlock);
    if (kHeader->state != static_cast<unsigned long>(BlockState::kLive)) {
        return false;
    }
```

头一道闸问的是地址:指针退回 32 字节拿到的是块头,它落在 span 里吗,不在的就是野指针,原样退了回去。第二道闸问的是状态:头的 state 得是忙的,闲着的(比如这块早就还过了)就是双重释放的苗头,这一关也把它拦下了。验完了身,咱们看两边的邻居:后邻是空闲的,吞了,前邻要是闲的,就把自己并了过去。要是连前后邻一起都闲了,三块就并成了一块。找前邻家门的那一步,咱们抄下来的原文就三行:

```cpp
    unsigned long target = kBlock;
    if (kBlock > base_) {
        unsigned long const kPrev = kBlock - read_prev_span(kBlock);
```

一块的物理前邻在哪,没人直接记着——可前邻的尾巴紧贴着本块的头,read_prev_span 读出前邻的跨度,咱们拿它一减,前邻的家门就找到了。回走的本事,全在那 8 字节的尾巴身上。

grow 是最后一笔:铺子顶上的新空间,整段并了进来。它跟 free 的合并走的是同一套邻居逻辑——顶上那块要是本来就闲,新到的地界直接并进去,连链都省了重挂。什么时候会有新空间到顶?进货的活在下一节,咱们这里把接口立好放着:`grow(new_top)`,新顶必须在旧顶的上头,而且够一个最小块。

设计说完了,咱们交个底:本节咱们自己按住了三个真 bug,全是在长凳上、按在 host 的测试里,活着溜进内核的一个都没有。头一个出在取整的语义上。负载取整咱们调的是 base 的数学轮子 Ceil,可 Ceil 的语义咱们理解岔了:它数的是“能装下几个单位”,拿 64 字节、16 字节的单位宽度进去,它答的就是 4——四个单位,不是取整后的字节数。头一版直接拿这个 4 当了负载用,偏偏又碰上“小于最小负载就抬到 16”的下限,64 字节的请求出门变成了 16 字节的块,负载凭空缩了四分之三。“allocate 花掉多少空闲字节”的断言在长凳上当场把这笔错数按住。咱们的修法是补一层换算,修好的取整函数拢共三行:

```cpp
unsigned long round_payload(unsigned long bytes) {
    unsigned long const kUnits   = cinux::base::math::Ceil(bytes, kPayloadAlign);
    unsigned long const kRounded = kUnits * kPayloadAlign;
    return kRounded < kMinPayload ? kMinPayload : kRounded;
}
```

第二行把单位数乘回了单位宽,得到的才是字节,头一版错的正是少了这一乘,第三行顺带把下限也抬了。再一个是漏记:合并邻居的时候,被吞掉的邻居那 40 字节头尾开销,本来就是从空闲里扣出去过的,并回来时忘了记回去,free_bytes 就越合越少了。第三个最阴:一块被前邻吞并了之后,它自己的头忘了标成空闲——物理上它已经是前邻的一部分了,可它的头还挂着“忙”的字样,谁要是照着旧的指针再来一次 free,验身的一关居然放行,双重释放就从眼皮底下溜走了。三案的断言后来都长在了 test_heap 里,往后咱们谁再犯一回,长凳当场就红了脸。

host 的六案咱们列一遍名字您就有底:init 一段整仓、allocate 对齐且花数对得上,free 三块全合回去、first-fit 挑最低的洞,咱们连野指针都一律拒收,grow 顶上接的是新粮。铺子的算术,长凳上就这么验瓷实了。而算术再对,铺子里还没有一粒米——进货的页要等下一节才到。
