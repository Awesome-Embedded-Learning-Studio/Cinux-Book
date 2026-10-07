---
title: 07 · 一位之差
description: "扫描码是键位的地址,不是字符。set 1 的松开码就是按下码的最高位,一张 MASK 按下松开分家——这就是选它的理由。两张 64 项的 constexpr 表,make 码当下标,shift 选表,两个修饰键折叠成一位,0xE0 扩展组整组丢弃也有状态机。翻译机实现沉在 .cpp,长凳直链编得动,五桩验语法。"
chapter: 10
order: 7
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - keyboard
  - scancode
---

# 一位之差

扫描码是键位的地址,而不是字符。键盘上报的是哪个座位动了,座位到字符的翻译归咱们——同一块键盘换一副刻字、QWERTY 变成 AZERTY,动的码一个也没有,改的只是词典。咱们收的码是 set 1,8042 的 bit6 替咱们翻好了。为什么偏要 set 1?它有一条别的码集没有的漂亮语法:松开码,就是按下码的最高位置一。0x1E 按下是 a 的座位,0x9E 是同一个座位的松开。一位 MASK 就把按下松开分了家——本节章名说的就是它。

咱们把词典立成两张表,住的地方在 scancode.hpp,写成了 inline constexpr,公告的面上人人可读:

```cpp
inline constexpr char kScancodeLower[kScancodeGlyphs] = {
    0,   0,   '1', '2', '3', '4', '5', '6', '7',  '8', '9', '0',  '-',  '=', '\b', 0,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o',  'p', '[', ']',  '\n', 0,   'a',  's',
    ...
};
```

下标用的是 make 码,64 项的长度,零表示这个座位没有可印的脸。表里躺的可不全是字母数字,您看 0x0E 记的是 '\b',0x1C 记的是 '\n',0x39 是空格——翻译机交出去的是控制台语法的字符,上一节格子的走法表认识它们。tab、ctrl、alt 记的是零,本卷的活轮不到它们。CapsLock 连表里的位都备着,只是眼下没人来读——留的口,哪天要做它了,词典是现成的。咱们的第二张表 kScancodeUpper、同样 64 项,shift 按着的时候查它:同一个座位,给的是另一副字面,'a' 变成了 'A','1' 变成了 '!'。

咱们把翻译机本体沉在 scancode.cpp——实现全藏 .cpp 的家法,公告头留的只是声明。它的全部身子:

```cpp
char TranslateScancode(TranslatorState& state, uint8_t raw_byte) {
    if (state.extended) {
        state.extended = raw_byte == kScancodeExtendedLeader;
        return 0;
    }
    if (raw_byte == kScancodeExtendedLeader) {
        state.extended = true;
        return 0;
    }
    bool const    kRelease = (raw_byte & kScancodeReleaseBit) != 0;
    uint8_t const kMake    = raw_byte & static_cast<uint8_t>(~kScancodeReleaseBit);
    if (kMake == kScancodeShiftLeft || kMake == kScancodeShiftRight) {
        state.shift_held = !kRelease;
        return 0;
    }
    if (kRelease || kMake >= kScancodeGlyphs) {
        return 0;
    }
    return state.shift_held ? kScancodeUpper[kMake] : kScancodeLower[kMake];
}
```

咱们的状态就两位:shift 按没按、上一字节是不是 0xE0。走的是纯函数的路:进的是字节,出的是字符,设备与队列是一概不沾的,咱们在长凳上直接编得动。流程咱们从上往下看。头一件事是吞 0xE0 的尾巴:见 0xE0 立旗,旗立了,下一字节无论是什么整口咽下,旗收——除非来的又是一个 0xE0、那旗接着立,这是连续两组的边界。而后用一位 MASK 分家:座位号掩掉最高位,归了位。修饰键的活紧跟着办:两个 shift、左 0x2A 右 0x36,两个座位折成了一位——谁按都算数,谁松都散了场,右边那颗的松开也一样回落。修饰键自己是不产事件的,它们改变的是查哪本词典。松开的普通键无事件,事件只在按下的那一拍发生。最后查的是表:shift 选表、座位作的是下标、查到零就交零,咱们不硬造。

0xE0 这组咱们多说两句,因为按了没反应的病里它占一种,而且是最冤的一种。方向键、小键盘的回车、右边的 ctrl 和 alt,进门都揣着一个 0xE0 前缀——这是 set 1 对第二副键盘的记法。这一卷咱们整组丢弃,当年第一遍也是有意丢的:按了方向键没动静,这是设计好的,而不是失灵。丢弃也有它的章法:状态机立旗吞字节、一组不多一组不少。等将来真要方向键了,比如给命令行挪光标的活,这组再立正经的语法,旗已经立好在那儿了。

咱们在长凳上立了五桩围着它转:表里按住四个键位,0x02 出的是 '1',0x1E 出的是 'a',0x39 出的是空格,0x35 出的是 '/'。平键走的是小表,双 shift 的折叠连右松开回落一起验、松开无事件,0xE0 组整吞——吞完紧跟着的普通键还得正常出字,证明旗真收了。咱们让测试直链 scancode.cpp,不连键盘的设备壳——翻译从此是件独立的家当、谁都能借。

字节能翻成字了。可字不能堆在中断的手上等——中断是传递,而不是仓库。下一节咱们立队列,接的是 1 号线、收一行回音。
