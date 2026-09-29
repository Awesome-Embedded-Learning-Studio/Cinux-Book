---
title: 05 · 模式列表,逐个问详情
description: "远指针拍平成平坦地址,模式列表原来住在调用方缓冲区自己的尾巴上;0xFFFF 终止符、128 项护栏,还有 32 位想要、24 位保底的双档挑法。"
chapter: 3
order: 5
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - vesa
---

# 模式列表,逐个问详情

三道问答的零件咱们都备齐了,现在就把它们装配起来了。下面这一段写在我们自己的 `stage2.cpp` 里,咱们顺着它把挑模式的过程走一遍。

## 模式列表住在哪儿

咱们问完 0x4F00 之后,`g_vbe_info` 里头就躺着一个远指针了:段和偏移各占 16 位,它指向一片 16 位模式号的数组,那一片数组到了末尾就拿 `0xFFFF` 收尾。咱们关心这片列表住在哪儿,于是在 QEMU 里拿 monitor 把活内存读了一遍:`g_vbe_info` 落在 `0x91C0`,而它那个偏移字段报回来的地址是 `0x91E2`,段值报的是零。`0x91E2` 就在咱们自己递进去的那块 512 字节缓冲区里头,从缓冲区开头数过去 0x22 个字节的地方,正好是 `g_vbe_info` 身子后头的尾巴——SeaBIOS 是把模式号列表当场写进了调用方这块缓冲区,并不是从 ROM 里另搬了一份出来。咱们要把这个远指针拍平成自己能用的地址:

```cpp
// NOLINTBEGIN(performance-no-int-to-ptr)
// Real-mode seg:off far pointer flattened to a DS=0 linear address; the BIOS owns the target.
auto const* mode_list = reinterpret_cast<unsigned short const*>(
    (static_cast<unsigned long>(g_vbe_info.mode_list_segment) << 4) +
    g_vbe_info.mode_list_offset);
// NOLINTEND(performance-no-int-to-ptr)
```

咱们看代码块里那行 `the BIOS owns the target`:它告诉咱们,列表内容由 BIOS 当场填出来、咱们只当读者,并不是说列表住在别处的。咱们把段值左移 4 位再加偏移,玩的还是老算术。这一步在本环境里看着像白做:段值报的是零,左移 4 位乘出来的还是零,加不加结果都一样了。可咱们不能因为这个就把乘法省掉。列表的段值和偏移都是 BIOS 填进来的,换一块固件、或者哪天调用方把缓冲区挪去了别处,段值随时都可能变成非零的了。咱们把缓冲区放到线性 `0x10000` 再问一次,同一个 QEMU 上 SeaBIOS 回给咱们的段值就是 `0x1000`,省掉乘法的写法当场就要出错了。动工前那次试写探测的时候,咱们验的是另一件事:九十三个号全数都读得回来,`0xFFFF` 那个终止符也老老实实待在了末尾。

咱们看上面那对 NOLINT 注释,在这儿一并交代一句就好了。把整数转成了指针,在一般的 C++ 里就是警报,clang-tidy 见了一个就要咬上去。可咱们这里的 cast,是实模式里读 BIOS 列表的唯一姿势,缓冲区里那几个字段都是 BIOS 在运行时填进去的,不是编译期的常量。所以咱们用块级的 NOLINTBEGIN/END 把它圈住了,咱们在注释里写明了缘由,哨兵接着查它别处的毛病。最早那版写的是 NOLINTNEXTLINE,结果它跟违规行之间被一行 why 注释隔开了,它指的下一行成了注释本身,那个告警照样响了。上过一回当的写法,现在咱们统一改成块级了。

## 问价循环

```cpp
constexpr unsigned int kMaxModesToProbe = 128;
unsigned int           seen             = 0;
unsigned short         picked32         = 0xFFFF;
unsigned short         picked24         = 0xFFFF;
for (unsigned int i = 0; i < kMaxModesToProbe; ++i) {
    unsigned short const kNumber = mode_list[i];
    if (kNumber == 0xFFFF) {
        break;
    }
    ++seen;
    if (!bios::QueryModeInfo(kNumber, &g_mode_scratch)) {
        continue;
    }
    if (picked32 == 0xFFFF && MatchesRequest(g_mode_scratch, 1024, 768, 32)) {
        picked32      = kNumber;
        g_mode_chosen = g_mode_scratch;
    } else if (picked24 == 0xFFFF && MatchesRequest(g_mode_scratch, 1024, 768, 24)) {
        picked24 = kNumber;
        if (picked32 == 0xFFFF) {
            g_mode_chosen = g_mode_scratch;
        }
    }
}
```

<Anim id="vesa-enumerate-loop" />

动画里咱们把问价的结果摆成了一张清单:红的几行对不上,绿的那行才是宽高、色深、线性帧缓冲都占齐的。

咱们从头顺着读。列表的收尾靠 `0xFFFF` 这个终止符,可万一哪家 BIOS 忘了写呢?`kMaxModesToProbe` 就是为这一手准备的护栏,咱们不赌它,顶多就挨个问满 128 个号了。QEMU 的九十三个离上限远着,这个数防的是没了终止符时的一路狂奔。`0xFFFF` 还兼了另一份差事:它不是合法的模式号,所以咱们拿它当还没挑中的记号,`picked32`、`picked24` 两个档位的缺省值都是它。

咱们每问一个号,`QueryModeInfo` 就会把详情写进 `g_mode_scratch` 的肚子里。要是问失败了呢?咱们就 `continue` 一下,跳过去问下一个号了。这里咱们是故意不 `fail` 的:列表里混着几个 BIOS 拒答的号都很正常,一个模式问不出详情了,不代表排在它后面的也都没有。真正致命的是循环走完,咱们一个能用的都没挑到,那才是循环外那句 `fail("VESA mode")` 的语义。

判定本身咱们只写了一行,`MatchesRequest(g_mode_scratch, 1024, 768, 32)`,它查的是一组条件,属性位的 bit 7 得置着,那就是本模式支持线性帧缓冲的招牌,宽、高、色深咱们逐项比对,最后 framebuffer 的地址还得非零。旗插着、地址却是 0 的,咱们按坏了处理。这个函数的家在 `vesa.hpp`,它是 constexpr 的,一行逻辑都没有碰过 BIOS 那边的事,所以 host 世界能拿它做测试,下一节咱们就是这么办的。它对着的是上一站的 `ClassifyEntry`:循环住在动作件里头,判定逻辑则住在头文件里头了,上一站说好的分工,咱们原样又用了一遍。

## 双档挑法

为什么挑两档?动工前那次试写探测给过咱们底气:QEMU 报上来的列表里有 1024×768×32 的 `0x144`,32 位色自然是咱们的首选。可咱们又不能假设全天下的卡都有它,万一是台只有 24 位货的老卡呢?所以策略写成了这样:32 位、24 位两个档,各记各的头一个中选号。循环走完了,32 位档有货咱们就用 32 位,没货咱们就用 24 位保底。

档位的次序里还有个小机巧。24 位的中选会赶早一步住进 `g_mode_chosen`,可后面要是又问出了 32 位的,32 位的分支就会把它覆盖掉。保底的住进来,更优的到了就换,次序恰好向着咱们。您把循环里那句 `if (picked32 == 0xFFFF)` 的守卫看一眼就明白了:32 位咱们只收头一个,24 位咱们也只收头一个,谁后到都不挤掉同档的前辈,只有 32 顶 24 的单向通道。

## 切过去,存下来

咱们把两个档位合成一个中选号之后,剩下的就只剩两下收尾了。咱们调 `SetVideoMode`,函数内部自动把 bit 14 或进了模式号,调用侧连这个位都不用惦记了。切换成功了,咱们再把 `g_mode_chosen` 的五个字段抄进 `g_framebuffer` 存档。咱们留心这个次序:切换成功以前,任何一步的失败都走 `fail` 通道,存档里就落不下半套数据了。存档装的是切换成功之后那几个字段的原文。
