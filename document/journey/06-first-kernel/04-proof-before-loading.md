---
title: 04 · 上限是验出来的
description: "没有预设的最大内核尺寸,只有可装载性证明:九道检查从魔数一路到脚印重叠,区间包含而不是总量求和;当年内核压着自家栈崩在一个 ret 上的事故,如今是进场前的最后一判。"
chapter: 6
order: 4
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - loader
---

# 上限是验出来的

上一节咱们自己给自己留了道题:鞋盒上的数,全是内核自己盖的章,boot 凭什么信?当年第一遍的答案是干脆不信——在 boot 里写死一个最大的内核尺寸,镜像敢超了就喊停。听着倒是稳妥,可上限是个拍脑袋的数,内核真长过了它,要么回头改 boot 重编,要么把上限抬成一个谁也用不满的大数。更要紧的是,它看住的只有大小这一样:尺寸合格、住址却报进别人院子的鞋盒,照样过得了它这一关。

这一遍咱们把答案换个方向:上限不设了,设的是证明。装载动手之前 boot 拿鞋盒上的声明,对着两份现实各问一遍——内存里真有这一整块地吗?这块地跟 boot 自家的脚印打不打架?加上鞋盒自身的算术检查,一共凑足了九道。任何一道要是过不了,当面拒了,面板上留一行拒绝的理由——一个扇区都不动。判官的住处也在 `image_header.hpp`,头两道是这样的:

```cpp
constexpr LoadStatus ValidateImage(const ImageHeader& header, const Region* usable,
                                   unsigned usable_count, const Region* boot_owned,
                                   unsigned owned_count, uint64_t max_paddr = 0x100000000ULL) {
    if (header.magic != kImageMagic) {
        return LoadStatus::kBadMagic;
    }
    if (header.version != kImageVersion) {
        return LoadStatus::kBadVersion;
    }
```

后面的七道,咱们按次序数。镜像不许是空的,而 mem_size 不许比 file_size 短。两笔加法不许回卷:load_paddr 加上了 mem_size,file_size 加上了 511——两笔共用的是同一道判,谁加冒了都算这一关的罪。u64 的加法加冒了会从头再来,和反而比加数小,所以判回卷就认这桩怪事。后一笔是扇区算术马上要用的加法,咱们在这儿验掉,后面才敢放心地用。再有就是 max_paddr 那道 4GB 的顶。摆渡的 32 位平坦段够得着的高度就到这儿,鞋盒把家报到了 4GB 以外,算术再漂亮咱们也拒,船开不到那儿了。末了,门牌必须落在自家地界 [load_paddr, 尾巴) 这个半开区间里——尾巴取的是 load_paddr 加 mem_size,半开的意思就是上沿不含,顶到尾巴上的门牌不算数。您数一下,到这儿七道了,用到的全是纯算术和比大小,咱们连一次数组都没碰。

第八道轮到的是内存现实,这也是 E820 图谱头一回办了正事。判法是 ContainsRegion:图谱里必须有一条现成的可用区,把从 load_paddr 到尾巴的整个区间从头到尾罩住。为什么不把可用内存加一加、总数够就放行?因为镜像是一整块连续的字节,它没法分头住进两块不挨着的地——图谱上的那些洞,总量帮不了忙。咱们手头的 QEMU,最大的可用区从 1MB 起一大片,2MB 的家稳稳当当躺在它怀里。可哪张鞋盒要是把家报进了 0xA0000 那个洞里,这一道就把它拦下了。

次序是排过的:最便宜的问话放在头里,两道扫描压在了最后。咱们犯不着替一张胡写的鞋盒花扫描的功夫。这九道也是拒绝的次序——面板那行 rejected: 后头跟的人话,报的就是头一道没过的关。

压轴的一道是脚印,它是拿一次真实的事故换来的,咱们去考古箱里翻那一趟。当年的第一遍,内核装在了 0x10000 到 0x90000,而栈是一枚 SS=0x0900 的实模式段:段基落在物理 0x9000,一个段足足能探出 64KB 的地界:从 0x9000 一直伸到 0x19000,栈顶就顶在了 0x18FFE。装载区把这段栈的上沿整个盖了进去,两家共用的地界从 0x10000 到 0x19000,不多不少的 36KB,重叠的这笔咱们在问内存那一卷拿段的算术算清过。读盘全程都成功了,放心也就跟着来了,然后机器就崩在了一个 ret 上:读盘是一趟静态写入,它不认得栈上还压着活着的返回地址,照单全写、盖了个干净。笔者当年盯着读盘的代码查了半天——读盘没有毛病、一个字节都没读错,毛病在从头到尾没有人问过一句:内核要住的地界,跟栈打不打架?崩点不在读盘——在踩栈。

到了这一遍,四块脚印进了判据。咱们到 `boot/stage2.cpp` 里把它摆开:

```cpp
cinux::boot::Region const kOwned[] = {
    {.base = 0, .top = kFerryWindow + kFerryWindowSize},
    {.base = kStage2Spot.offset,
     .top  = kStage2Spot.offset + cinux::boot::load::SectorBytes(kStage2Spot.sectors)},
    {.base = kPageTables.base, .top = kPageTables.top},
    {.base = kStage2Stack.top - 0x1000, .top = kPmStackTop},
};
```

低址连同暂存窗一整片、stage2 本体的扇区、三张表盘,还有栈的那一竖条。鞋盒声明的地界跟四块里任何一块相犯,九判的最后一道 kBootOverlap 照样拒载。当年那个崩在 ret 上的晚上,要是过的是这一关,面板上留下的是一行 overlaps boot footprint,机器还好好地活着,笔者也能早点睡。

拒绝也得有拒绝的姿态。`NameOf` 给每种结局配了一句人话:bad magic、overlaps boot footprint、beyond 4G ferry reach……咱们真造过几张胡写的鞋盒去试:家报进洞里的,拒了。压着 stage2 院子的,拒了。报到 4GB 外的,拒了。张张当面退了货,面板上留了话,机器安安稳稳地停在 Halt 里。跟上一卷连遗言都不留的三重故障比,这一卷连拒绝都是体面的。还有一层设计上的便宜:九判写成了不碰任何硬件的 constexpr 纯函数,如今 host 的测试里就是这么干的——九种拒绝挨个喂参数,演了一遍,连虚拟机都省得开了。

验过了,才轮到咱们动手搬。读头、验、循环摆渡、回执——咱们下一节看这四段怎么接成一条流水线。
