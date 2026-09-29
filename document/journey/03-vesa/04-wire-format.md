---
title: 04 · 两块缓冲,十七个字节
description: "VbeInfoBlock 与 ModeInfoBlock 的镜像结构、尾巴为什么要补齐、offsetof 断言怎么守住 BIOS 的落笔位置,以及 FrameBufferInfo 里整体对齐的新花样。"
chapter: 3
order: 4
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - vesa
  - struct-layout
---

# 两块缓冲,十七个字节

BIOS 往咱们递的房子里写东西,这已经是第二回了。上一回的 `MemoryMapEntry`,咱们立过一套完整的防线:packed 守住身材,offsetof 的断言守住每个字段的落点。这一回咱们照方抓药,只是对象换成了两位新住户,还多出一位自带新花样的。

## 镜像结构:只挑要用的,尾巴补齐

`VbeInfoBlock` 的身材是 512 字节,它是 0x4F00 的收货区。`ModeInfoBlock` 的身材是 256 字节,它是 0x4F01 的收货区。两块缓冲的完整字段表都长得吓人,咱们一个都不全要:控制器那块,咱们取的是版本和模式列表的指针。到了模式详情那块,咱们取了属性位、宽、高、色深、行距、framebuffer 地址,拢共也就取了六样。遇到不认识、不消费的字段,咱们一律在结构里用占位数组跳过去:

```cpp
struct [[gnu::packed]] ModeInfoBlock {
    unsigned short attributes;  // bit 7 = LFB supported
    unsigned char  reserved_02[0x0E];
    unsigned short pitch;  // bytes per scan line
    unsigned short width;
    unsigned short height;
    unsigned char  reserved_16[0x03];  // char cell, planes
    unsigned char  bpp;
    unsigned char  reserved_1a[0x0E];  // memory model, masks, ...
    unsigned int   framebuffer;        // physical LFB base
    unsigned char  tail[256 - 0x2C];
};
```

咱们看头尾两处细节。字段中间的那些 `reserved` 数组,跳过的是 BIOS 布局里咱们不读的字段,好让后面的成员落在正确的偏移上。结尾的 `tail` 是本站的新成员:4F01 的约定写的是往这块缓冲写满 256 字节,咱们的结构要是声明到 `0x2C` 就收尾,BIOS 照样按它的约定一路写满,越界写进 `.bss` 里的邻居。上一站 `g_memory_map` 旁边还是空的,到了本站就往里搬进来几个新住户:控制器信息 `g_vbe_info`、问价用的草稿 `g_mode_scratch`、挑中模式的快照 `g_mode_chosen`,还有最后的帧缓冲存档 `g_framebuffer`。咱们把尾巴补齐到 256,BIOS 写到头也还在咱们自家的院子里。

老一套咱们一条不落全都要:

```cpp
static_assert(sizeof(ModeInfoBlock) == 256);
static_assert(__builtin_offsetof(ModeInfoBlock, attributes) == 0x00);
static_assert(__builtin_offsetof(ModeInfoBlock, pitch) == 0x10);
static_assert(__builtin_offsetof(ModeInfoBlock, width) == 0x12);
static_assert(__builtin_offsetof(ModeInfoBlock, height) == 0x14);
static_assert(__builtin_offsetof(ModeInfoBlock, bpp) == 0x19);
static_assert(__builtin_offsetof(ModeInfoBlock, framebuffer) == 0x28);
```

这些偏移咱们为什么一条条守死?因为它们全是 BIOS 的落笔位置,咱们写错一个数字,读出来的就是隔壁字段的值,编译器倒是一句怨言都没有。断言的家就在头文件 `vesa.hpp` 里,boot 世界的 -m16 编它一遍,host 世界的 -m64 再编它一遍,两个 ABI 是各站一班岗的。

## 行距:读字段,别算乘法

pitch 说的是每行像素在显存里占多少字节,真正拿主意的只有 `0x10` 偏移上填的那个数。您可能手痒,想拿宽乘色深再除以 8 自己算了。咱们劝您别这么干:显卡往一行的尾巴上补对齐,那真是家常便饭了。1024 像素宽、32 位色的模式,一行占了 4096 个字节,恰好等于咱们算出来的数,它要是占了 8192,那也完全是合法的。VBE 3.0 的字段表里甚至备了两个行距,banked 的和线性的各一个。本环境里 SeaBIOS 给 `0x10` 和线性行距填上了同样的 4096,所以读 `0x10` 拿到的就是它。咱们读 `0x10` 偏移上 BIOS 填的那个,它填多少咱们就信多少,自己一个乘法都不去做了。收工面板上咱们看到的那个 `pitch 4096`,就是它的原文。

## FrameBufferInfo:漂移换了漂的地方

咱们自家的存档 `FrameBufferInfo` 不是线格式,它只有五个字段:排在最前面的物理地址占掉了 8 个字节,后面的行距、宽、高、色深则依次跟上来。上一站 `MemoryMapEntry` 的教训您还记得,`unsigned long` 在两个世界里的宽度不一样,所以咱们禁了它,32 位的字段一律改成 `unsigned int`。这一回咱们把字段类型全挑成了双世界同宽的,按说该太平了。

可动工前那次试写探测的时候,断言还是响了。这回漂的东西换了,不再是字段的宽度,而是整体的对齐,咱们把 `FrameBufferInfo` 在两边的尺寸都量了一遍。量到 `physical` 这个 8 字节的 `unsigned long long` 时,咱们盯的是它要落在几的倍数上。boot 这边是 i386 的 ABI,它只要 4 的倍数就够,到了 host 那边的 LP64,这个数换成了 8。同一个结构体量了两遍,unpacked 的身材就不一样了:boot 世界数出来了 20 个字节,host 世界也被补齐到了 24,而咱们想守住的 sizeof 是 17。咱们给它挂上 packed 之后,两边才都是实打实的 17。

```cpp
struct [[gnu::packed]] FrameBufferInfo {
    unsigned long long physical;
    unsigned int       pitch;
    unsigned short     width;
    unsigned short     height;
    unsigned char      bpp;
};

static_assert(sizeof(FrameBufferInfo) == 17);
```

上一站咱们说过,`MemoryMapEntry` 挂 packed 属于提前上的门闩。到了本站,packed 干的是实活:跨世界共享的结构,整体的对齐也会漂,所以凡是 boot 和 host 两个世界都要编的结构,packed 就不再是可选项了。执法的还是那套 sizeof 断言,外加咱们让测试流水线把头文件拉进 LP64 世界重编一遍。咱们要的是同一个 17,量出来了 20 和 24,断言报回来的就是它们对不上。
