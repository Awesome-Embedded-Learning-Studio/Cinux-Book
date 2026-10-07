---
title: 02 · 一盒字模,住进镜像
description: "画面是像素的,字母不是:8×16 的经典点阵按 PSF2 装盒,4128 字节,32 头加 4096 本体。ParsePsf2 拒答四病,全零安全退化,字模坏了不带崩内核。objcopy 把 font.psf 变成 .rodata.font,KEEP 挡住回收,链接期改符号名入家法,小端读词立进 base。长凳九桩用合成字体,真字体归开机。"
chapter: 10
order: 2
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - font
  - psf2
---

# 一盒字模,住进镜像

画面是像素的——字母不是。轮到一个 h 上屏的时候,咱们得有它的模样打底:十六行、每行八点,哪些点亮哪些不点——描一个字的点阵叫字模,整套字符的字模合起来叫字体。PC 世界给控制台字体立过一套干净的格式规范,咱们认的叫 PSF2,咱们的字模就按它装盒。数据手册里的字段列得整整齐齐,咱们只认用得着的四个数:资料不背,讲清楚就够了。

PSF2 的头 32 个字节,起手的是一个魔数 0x864AB572,小端躺在盘上——配屏那一卷立过的信条在这儿照样管用,对上了暗号才谈内容。头里咱们用四个数:字模的个数 256、每个字模 16 字节,摊成高 16 行、宽 8 点的格子。本体就是 256 乘 16 的 4096 个字节:一个字符占一段,一个字节管的是一行,一个像素认的是一位,最高的位排在最左。整个文件的 4128 字节,是 32 加 4096 来的,数是齐的。字模用的是 8×16 的经典 IBM 点阵,排布走的 CP437——当年 PC 一开机满屏就是它。横平竖直的字,一笔是一笔的干净。

字模的家安在 assets/font.psf,生成它的是 scripts/gen_psf_font.py:4096 个点阵字节靠手点是点不出来的,脚本里内嵌的是整套点阵,咱们跑上一遍,文件就出来了——永续再生。定稿之前咱们还请过一位笔画细些的候选来试装,两版各点亮了一回,摆在一起拿肉眼比了一轮,还是留下了经典原版——细是真细,可黑底白字的世界里,粗一点的笔画才站得住。当年第一遍用的也是它:血统就没换过。

内核拿到了一盒字节,咱们头一件事是验货。ParsePsf2 是个纯函数——咱们让它四病拒答,咱们把正身摆出来:

```cpp
    font.glyph_count = cinux::base::ReadWord32(data, 16);
    font.charsize    = cinux::base::ReadWord32(data, 20);
    font.height      = cinux::base::ReadWord32(data, 24);
    font.width       = cinux::base::ReadWord32(data, 28);
    uint64_t const kBodyBytes =
        static_cast<uint64_t>(font.glyph_count) * static_cast<uint64_t>(font.charsize);
    bool const kShapeOk = font.width != 0 && font.width <= kPsf2MaxWidth && font.height != 0 &&
                          font.charsize >= font.height && font.glyph_count != 0;
    font.valid          = cinux::base::ReadWord32(data, 0) == kPsf2Magic && kShapeOk &&
                          static_cast<uint64_t>(kPsf2HeaderBytes) + kBodyBytes <= bytes;
    if (!font.valid) {
        font = Psf2Font{};
    }
```

四病咱们一样一样数:文件比头还短、魔数对不上,宽度超过了 8,尺寸的数凑不齐——头加本体超过了文件本身。拒答的方式是整个归零:valid 是 false,其余的字段全空着。为什么拒答而不报错:字模是数据不是代码,数据坏了,总不该把内核带崩吧。字模一旦闭了嘴,画面那一路从此就哑了,串口照旧扛着全部的输出,保险咱们是按分层买好的。宽度封在 8 也是有讲究的:一行一字节,整个取位的语法就小。真到了要宽字体的那天——拼一行要用掉好几个字节,那就成了另一套语法,等真消费者出现了再说,咱们不提前备货。

取位的两下,咱们都放在头文件里,是长凳直接编得动的纯度。GlyphRowBits 算行字节的落点:头 32 字节,加字模号乘上的 charsize、再加行号。而 GlyphPixel 从行字节里抽一位:

```cpp
constexpr bool GlyphPixel(unsigned char row_bits, uint32_t column) {
    return ((row_bits >> (kPsf2MaxWidth - 1U - column)) & 1U) != 0;
}
```

列零对应最高位——最左边的像素。格式里写死的:左就是左,咱们不猜。

字模怎么跟着内核走?它得进镜像,咱们还得让链接器认得它。boot 那边办过一回同样的事:objcopy 把裸文件变成一个目标文件,链接期就挂进了段里。内核这边照办的时候,还多讲究了两步,咱们把 CMake 里的一串命令摆出来:

```cmake
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/assets/font.psf
            ${CMAKE_CURRENT_BINARY_DIR}/font.psf
    COMMAND ${CMAKE_OBJCOPY} -I binary -O elf64-x86-64 -B i386:x86-64
            --rename-section .data=.rodata.font
            --redefine-sym _binary_font_psf_start=g_font_psf_start
            --redefine-sym _binary_font_psf_end=g_font_psf_end
            --set-section-flags .rodata.font=alloc,load,readonly,data
```

咱们把文件抄到构建目录再变,是因为二进制符号名是从文件路径铸出来的:绝对路径一长,符号名跟着就出了格。段名改成了 .rodata.font,是给它一个自家的名字,链接脚本的 .rodata 里好写 KEEP(*(.rodata.font))——没有 KEEP,gc-sections 的那一扫,没人引用的字模整盒就当垃圾收走了。符号改名是家法的事:_binary_font_psf_start 不合 g_ 开头的命名,链接期一道 redefine 就办了,比在代码里挂豁免干净多了。flags 的四个词把身份说全:要占内存、要随镜像装车、只读、是数据。内核那头留了个小桥 font.cpp,留的两个函数:EmbeddedFont() 解析一次,EmbeddedFontData() 给出首字节——符号的事、到桥为止。

解析里用到的那个 ReadWord32,值得咱们单独说一笔。盘上的格式是小端,一个 32 位字是由四个字节拼成的,而这拼法是纯数学,不该是字模的私产——咱们把它立进了 base,起的名字叫 byte_order,注释里写着将来的消费者:ext2 的超级块、FAT 的表。新的纯函数落了位,咱们得问一句 base 是不是它的家。这个教训在串口那一卷交过学费:手搓的端口循环被点了名,这一回一次就到位了。长凳上 bits 那件还给它添了两根哨兵:一个魔数的字节序,一个偏移的语义。

长凳的字体测试九桩,咱们用的全是合成的小字体——constexpr 现造一个三五字节的小盒,拒答的四病各有一桩对号,取位的路数各有一桩验数。不依赖真资产是有意的:真字体四千个字节,咱们靠测试去读它,是读不出病来的——病都在语法里。真字体的实弹归开机:它要是坏在镜像里,屏幕上当场就见了分晓。

一盒字模上车了,墨备好了。可纸还没有——显存眼下只是一大片过了门却没语法的地址。下一节轮到咱们给像素立语法。
