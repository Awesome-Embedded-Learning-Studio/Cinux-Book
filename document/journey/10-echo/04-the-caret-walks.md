---
title: 04 · 光标的走法
description: "字符的世界不认像素,认格子:128 列乘 48 行,养一个光标。Caret 和 CaretStep 是纯函数的行列状态机,不变式立在最前——光标从不离开网格。换行回车退格各有走法,最后一列画完就换,底行换行就地滚屏。画字、步进、滚一字符行,三家在 put_char 里凑齐,长凳九桩穷举不变式。"
chapter: 10
order: 4
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - console
  - terminal
---

# 光标的走法

字符的世界不认像素,认的是格子。把画布切成 128 列乘 48 行的格子,咱们再养一个光标——下一个字落在哪儿。这一层咱们特意立成纯函数,住 console_grid.hpp——设备的字节一个不沾、长凳上随便编。咱们的状态就一对数——行和列,名字起的叫 Caret。一步的名字叫 CaretStep:下一个光标,配上的是一个布尔,说的是滚没滚。

咱们还没动手,不变式就立好了,比代码还早:不管进来什么字符,出去的光标永远满足列小于总列、行小于总行——光标从不离开网格。为什么咱们把它当命:光标一旦出界,下一笔就落进了别人的行甚至网格外。一笔错了还只是开头,从此的每一笔都跟着错,而且一句报错都没有。不变式立住了——出界这件事就从可能发生,变成了写不出来。

所有的走法都在一个函数里,咱们整个看:

```cpp
constexpr CaretStep AdvanceCaret(Caret current, uint32_t columns, uint32_t total_rows,
                                 char glyph_char) {
    if (glyph_char == kCharNewline) {
        return AdvanceLine(current, total_rows);
    }
    if (glyph_char == kCharReturn) {
        return CaretStep{.caret = {.row = current.row, .column = 0}, .scrolled = false};
    }
    if (glyph_char == kCharBackspace) {
        uint32_t const kBacked = (current.column > 0) ? current.column - 1 : 0;
        return CaretStep{.caret = {.row = current.row, .column = kBacked}, .scrolled = false};
    }
    if (current.column + 1U < columns) {
        return CaretStep{.caret    = {.row = current.row, .column = current.column + 1U},
                         .scrolled = false};
    }
    return AdvanceLine(current, total_rows);
}
```

换行的时候:列归零、行进一。打字机年代的回车和走纸是两件事,字符的语法里也是两个字符:'\r' 只回行首不动行、'\n' 只动行不回首,串口终端里这俩配合了五十年,咱们让格子照老礼数各管各的。退格的时候:列退一、退到墙就停,折行是不许的。可见字符来了:占一格、往前走。走到最后一列的时候,字就上了屏,光标当下就落到下一行的头上——换行趁早,不在界边上留悬着的尾巴,不变式也不点头的。而底行的换行不一样:再往下没有行了,AdvanceLine 就地报滚屏、行留在原处,等的是屏幕往上让位。

咱们把刚立的语法接到像素上,落脚的地方是 screen.cpp,三家人在咱们这儿凑齐:字模给的是模样,驱动出的是像素,格子定的是走法。init 的三步:显存那件起头,内嵌的字模解析一回存进成员,末了清屏。put_char 也是三步、顺序是写死的:

```cpp
    if (glyph_char != kCharNewline && glyph_char != kCharReturn && glyph_char != kCharBackspace) {
        paint_glyph(kGlyph, caret_);
    }
    CaretStep const kStep = AdvanceCaret(caret_, columns(), rows(), glyph_char);
    if (kStep.scrolled) {
        screen.scroll_up(font_.height);
    }
    caret_ = kStep.caret;
```

控制字符不画画——它们的使命就是挪光标,画了反而脏。可见的字符落在光标格上——然后步进,滚了就把整个屏幕往上搬一行字模的高、十六个像素行。字模的号,拿字符的字节值当:字模要是不足 256 个,界外的字符落 0 号空模,本卷带的字体是全的,这道闸咱们备给将来的小字体。画字的 paint_glyph 就两个循环:外头十六行,每行取的是一个字节,里头的八列,一位一位地问 GlyphPixel,亮的给墨,不亮的给纸。

128 乘 48 的格子,面板上打的正是它们——不过那得等装配完才见得着,是下一节的活。咱们在长凳上已经验了九桩,咱们把不变式穷举:四个角、最后一列、底行、退格到墙、底行写到最后一列再换,每一步出来都拿不变式卡了一遍。纯函数的世界里穷举是便宜的——便宜就该占足。

格子立好了,字也会走了。可这些字眼下从哪儿进来?输出的活,还全压在串口的一根线上,画面缺的是一张嘴。下一节咱们把控制台重新装配一遍:脸不动,加的是一张嘴。
