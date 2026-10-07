---
title: 03 · 显存的语法
description: "显存说穿了是一片内存:一个物理地址起头,每个像素一个 32 位字。单子上的六个颜色位域头一回被消费,ComposePixel 截位组色,QEMU 的排布是红 16 绿 8 蓝 0 高 8 位空。pitch 是行跨度不是宽度乘四,QEMU 里相等是巧合不是承诺。越界静默拒,清屏滚屏按行搬字,长凳四桩合成几何。"
chapter: 10
order: 3
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - framebuffer
  - driver
---

# 显存的语法

显存说穿了是一片内存:boot 报一个物理地址、从那儿起,每个像素占的是一个 32 位字——写像素就是往门里写一个字。门是上一节开好的,地址走恒等,跟咱们写自家数组一个手感。剩下的问题是语法:哪一个字、写给哪一个像素,写成什么颜色。

颜色的语法,原料就是单子上的六个位域。32bpp 说的只是一个像素 32 个比特,没说红绿蓝各占哪几个——配屏那一卷咱们挨过双重的打,所以誊单子的时候六个数一笔不落。它们在单子上躺了几卷——今天头一回有人消费:

```cpp
constexpr uint32_t ComposePixel(const cinux::boot::FramebufferInfo& layout, Paint paint) {
    uint32_t const kRed   = static_cast<uint32_t>(paint.red) >> (8U - layout.red_size);
    uint32_t const kGreen = static_cast<uint32_t>(paint.green) >> (8U - layout.green_size);
    uint32_t const kBlue  = static_cast<uint32_t>(paint.blue) >> (8U - layout.blue_size);
    return (kRed << layout.red_shift) | (kGreen << layout.green_shift) |
           (kBlue << layout.blue_shift);
}
```

开门那一章咱们打点看过的排布,红的 8 位在 16、绿的 8 位在 8、蓝的 8 位在 0,最高的 8 位空着——行话叫 XRGB8888。每条通道按掩码的宽度右移截位,而后落进自己的位置:8 位进 8 位,今天走的是原样。哪天遇上一条通道只有 6 位的卡,超出的低位自己掉,永远不会越了界。色板咱们只备两个词:墨、开的就是红绿蓝全开。纸、刷的就是全黑。画或者擦——本卷的画面语法全在两个动词里头。

位置的语法里,藏着新手最容易栽的一个等号。找第 y 行第 x 列的像素,直觉的写法是拿宽算:偏移等于 y 乘 width 乘 4,加的是 x 乘 4。这个公式在 QEMU 里恰好是对的:1024 乘 4 正好 4096。可对上靠的是巧合,承诺是谈不上的。行与行之间的字节距离——行话叫 pitch,是硬件给的,不是分辨率算的:硬件可以给行做衬垫对齐,pitch 大过 width 乘 4 也是合法的。init 那头咱们只拒 pitch 小于 width 乘 4 的,那是画不满一行的,更大的照单全收。真拿宽当跨度的公式,遇上衬垫的卡,每往下一行、整行就往左错了一截,拼出来的画是斜的,而且歪得特别齐整。它的正身:

```cpp
constexpr uint64_t PixelByteOffset(uint32_t pitch, uint32_t column, uint32_t row) {
    return (static_cast<uint64_t>(row) * pitch) + (static_cast<uint64_t>(column) * 4U);
}
```

设备本身是 Meyers 成员单例的又一户,起的名字叫 Framebuffer,家安在了 kernel/driver/,跟串口和定时器是老街坊了。init 吃单子里的 FramebufferInfo,四道拒辞:不是 32bpp、宽是零、高是零、pitch 撑不满一行的——占一道就整个拒收,基址留了空,设备从此就是惰性的。为什么拒而不崩?咱们在字模那儿认过同一个理:画面可以没有,内核是陪葬不起的。真没有画面的机器上,这一路输出全是安静的空转——串口那一路照说不误。

咱们把写的口子收成一个 put_pixel、门口带一道闸:

```cpp
void Framebuffer::put_pixel(uint32_t column, uint32_t row, bool lit) {
    if (base_ == nullptr || column >= width_ || row >= height_) {
        return;
    }
    *address_of(column, row) = lit ? ink_ : paper_;
}
```

出界的写,咱们当没听见。为什么静默:光标的算术住在上层,咱们不让驱动信任何上层的算术,只信自己门口的这道闸——上层真算错了,丢的是一笔像素,而不是一次开机。

咱们再给驱动派两件家务,大扫除的活和搬家的活。clear 干的是整面刷纸,一行一行地刷,每个字都换成了纸色。scroll_up 是行级搬字:第 lines 行起到末尾的整行上移,腾出来的底带刷纸。按的是行而不是字节,因为行就是 pitch 的单位,一行是一整段齐整的字。搬字的读写都走 volatile,显存是设备的内存——每一笔都得真落。这个搬法粗看是奢侈的,一屏是三兆字节的一次搬家。可它只在最底行写满的那一刻来一下——平时一笔也不白搬。

长凳立了四桩,咱们用合成的几何喂它:截位组色对着位域算期望值,越界的写一个也不落在邻居身上,滚屏前后咱们逐字比对。真实尺寸的实弹留到开机——三兆字节的画布,长凳上是养不起的,也不必养:语法对了,尺寸就只是个数了。

像素的语法齐了,墨与纸也有了。可 1024 乘 768 是将近八十万颗像素的画布——直接拿像素说话,谁也数不过来。字模是 8 乘 16 的:1024 除 8 是 128 列,768 除 16 得的是 48 行。下一节咱们在像素的上头,立一层行列的世界。
