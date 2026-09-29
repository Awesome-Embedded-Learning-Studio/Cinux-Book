---
title: 03 · 第二次进 BIOS 门
description: "4F00 递上 VBE2 签名拿列表、4F01 逐个模式问详情、4F02 带着 bit14 切过去;AX 全等 0x004F 的回执长什么样,和 E820 的 carry/SMAP 正好凑成对照。"
chapter: 3
order: 3
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - bios
  - vesa
  - inline-asm
---

# 第二次进 BIOS 门

咱们又一次站到了 INT 指令的面前。上一站问内存图的时候,咱们立过一整套待人接物的章程:参数都从寄存器递进去,回执也都过了几道闸才收,ES:DI 上的指针直传。好消息是 VBE 的章程几乎全套适用,咱们等于上了一堂复习课。真正的新面孔只有三样:bit 14 那个线性帧缓冲的开关、AX 全等 `0x004F` 的回执,还有调用前要亲手写上的 `VBE2` 签名。指针直传和 DS=0 那一套都是上一站留下的,这里原样接着用就是了。

## 三道问答各管什么

VBE 的服务全在 INT 10h、AX=0x4Fxx 的档子里,本站咱们用到三个。

头一问的 AX=0x4F00,问的是控制器。咱们递一块 512 字节的缓冲区过去,BIOS 就把这块显卡的自报家门写进来:VBE 的版本、总显存,还有咱们最惦记的那一样,它支持的模式号列表放在哪儿。这个调用的怪讲究藏在别处,咱们待会儿要单独说它。

第二问的 AX=0x4F01,咱们拿它查一个模式的详情。CX 里装的是模式号,咱们再递一块 256 字节的缓冲区过去,BIOS 就把这个模式的参数逐字段写进来:宽、高、色深、行距,还有 framebuffer 的地址。这样的查询,一个模式号咱们就问一回。

第三问的 AX=0x4F02,切的是模式。BX 里装的是模式号,不过咱们要在号上添一位再递。添的是 bit 14,数值 0x4000:它是线性帧缓冲的开关,置上了它,显卡给咱们的才是一整块连续的映射。忘了置它,模式号本身倒是照样切得过去,可咱们拿到的是 banked 模式,64KB 一扇窗口的老古董,后面画图的代码会一头雾水。所以咱们在头文件里给它起了名:

```cpp
/** @brief VBE mode-number bit that requests the linear framebuffer. */
inline constexpr unsigned short kLinearFrameBufferFlag = 0x4000;
```

<Anim id="vbe-three-asks" />

## 回执:AX 说实话,carry 不管事

上一站问 E820 的时候,咱们验的是 carry、SMAP 回执、写入字节数三道。VBE 换了副脾气:carry 咱们不看,标准认的只有一个数,咱们得看到 AX 全等 `0x004F` 才算。分开读的话,咱们递进去的功能号,回来的低八位 `AL` 还是 0x4F,就表示这块卡支持 VBE 的功能,AH 回来是 0 的话,就表示这次调用成了。AH 非 0 的时候,里面的值就是错误码。功能号没被认的时候,连 `AL` 也不会给咱们 0x4F。所以三个动作函数收尾的全是同一句:

```cpp
return register_ax == 0x004F;
```

咱们把 E820 那边和 VBE 这边摆到一起,差别就出来了:每种 BIOS 服务都有自己的撒谎方式。E820 的失败折在 carry 里,充数的手段还有只写 20 字节那一手。VBE 倒是干脆,它把一切都写在了 AX 里。相同的只有态度:回来什么都别急着信,咱们按对方自己的约定验过再说。

## VBE2 签名:调用方得表态

咱们必须在调用 0x4F00 之前,往 512 字节缓冲区的头四个字节亲手写上 `VBE2` 的四个字母。写了,BIOS 才按 VBE 2.0 以后的格式填这块缓冲区,模式列表的那些字段也才有货。不写的话,BIOS 只按 VBE 1.2 的老格式回,新字段全是空的。调用也还是不会给咱们报错,AX 照样回给咱们 0x004F,可咱们要的模式列表字段是空的。它静悄悄地就降了级,咱们防的就是这一手。

这又是咱们那句老话的新面相:BIOS 不是函数。它不光会干得比说好的少,还会反过来要求咱们调用方表个态。所以咱们把签名写进了 `QueryControllerInfo` 的函数体头一行,调用者想忘都没了机会:

```cpp
bool QueryControllerInfo(VbeInfoBlock* info) {
    info->signature[0]         = 'V';
    info->signature[1]         = 'B';
    info->signature[2]         = 'E';
    info->signature[3]         = '2';
    // NOLINTBEGIN(misc-const-correctness)
    // Operands bound to asm read-write/output constraints cannot be const.
    unsigned short register_ax = 0x4F00;
    // NOLINTEND(misc-const-correctness)
    asm volatile(
        "pushw %%ds\n"
        "popw %%es\n"
        "int $0x10"
        : [ax] "+a"(register_ax)
        : [info] "D"(info)
        : "memory");
    return register_ax == 0x004F;
}
```

## 指针直传,旧地址退休

两个要收缓冲区的动作函数,asm 块您扫一眼就眼熟了:上一站 ES 自救用的 `pushw %%ds`、`popw %%es` 咱们原样复用,`"D"(指针)` 把缓冲区的地址装进 DI。切换模式的 `SetVideoMode` 没有缓冲区要接,它只把 AX 和 BX 递了出去,指针那一套它用不上了。这是 DS=0 红利的第二回合,咱们的缓冲区住进 `.bss`,指针就是物理地址,ES 抄一遍 DS 就了事。

这里咱们得往回翻一页,看看当年头一遍是怎么放这几块缓冲的。上一站画地图的时候,咱们在图上圈过一块地,准备留给 VESA 的缓冲区,它的范围就是 `0x6000` 到 `0x6400`。

咱们翻回那一版看它怎么摆的。当年控制器信息、模式详情和 framebuffer 档案都住在写死的低地址上,咱们挨个点过去:`0x6000` 放控制器信息,`0x6200` 留给了模式详情,`0x6400` 存 framebuffer 的档案。这么安排是因为实模式下传指针要手工算段值,把地址右移 4 位再填进 ES,用几块写死的地址最省事。现在指针能直传了,缓冲区想住哪儿就住哪儿了,`0x6000`、`0x6200`、`0x6400` 三处地址,也连同了图上那块占位,本期一起退役掉了。E820 走过的 DS=0 路子,VESA 原样又走了一遍。

咱们在 `layout.hpp` 里,把 `kVesaBuffers` 这块占位整个删掉了。以前围着它的两条断言,现在合并成了一条直连:

```cpp
static_assert(kPageTables.top <= kStage2Stack.top);
```

咱们的图上从此就少了一块只挂着名字、没有住户的空地,每个区间的邻居都是真实住户了。上一站咱们提过,栈顶和 VESA 缓冲区之间还留着 3KB 的预算,那是照空置的预留量出来的。缓冲区搬进了 `.bss` 之后,这块地就收回了,那 3KB 的预算也不必再留。
