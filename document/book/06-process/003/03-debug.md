---
title: 03 · 调试现场
---

# 调试现场

这一章只有一条真实笔记,但它是个典型的「改一处不相关的代码、炸一处完全不相关的检查」,值得当案例。

## 案例:加 sync.cpp 让大内核进不去——mov rsp 的两种编码

症状挺唬人:`make run-kernel-test` 跑 mini kernel 测试,一路绿,打印 `=== MINI KERNEL TESTS PASSED ===`,然后突然冒出一行 `=== Loaded ELF is not a real kernel, exiting ===`,big kernel 测试**根本没执行**就退出了。mini 全过、big 没进,中间断在一个「入口魔数检查」上。

这个检查在 [main_test.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/test/main_test.cpp)。mini kernel 跳进 big kernel 之前,要确认入口处确实是「真内核」——它靠看入口字节:`_start` 的前两条指令是 `cli`(`0xFA`)+ `mov rsp, $__kernel_stack_top`。旧检查只认一种编码:

```cpp
// 旧: 只认 imm64 编码
bool is_real_kernel = (code[0] == 0xFA) && (code[1] == 0x48) && (code[2] == 0xBC);
```

`mov rsp, imm` 在 x86-64 有两种合法编码。`mov r64, imm64` 编码成 `48 BC <8 字节立即数>`(REX.W + 全宽立即数);而 `mov r/m64, imm32` 编码成 `48 C7 C4 <4 字节立即数>`(REX.W + 符号扩展的 32 位立即数)。GNU assembler 会看立即数范围自动选:当立即数(这里是 `__kernel_stack_top`)的低 32 位符号扩展后恰好等于 64 位值时,它就选更短的 `imm32` 编码(`48 C7 ...`),否则才用 `imm64`(`48 BC ...`)。

那为什么会突然换编码?根因正是本章那个看似无害的改动——把 `Spinlock` 从内联搬进 `sync.cpp`,等于多链了一个翻译单元,`sync.cpp` 里 `Mutex`/`Semaphore` 的静态成员、`g_pc_buf` 之类的全局让 **BSS 段长大了一点**。`__kernel_stack_top` 的链接地址随之微动,它的低 32 位恰好变成了「可以符号扩展」的那个范围,assembler 于是从 `48 BC`(imm64)换成了 `48 C7 C4`(imm32)。实际入口字节成了 `FA 48 C7 C4 00 00 02 81 ...`,而旧检查的第三个字节只认 `0xBC`,碰到 `0xC7` 就判「不是真内核」,直接退出。

修复是把检查放宽,两种编码都认:

```cpp
bool is_real_kernel = (code[0] == 0xFA) && (code[1] == 0x48) &&
                      (code[2] == 0xC7 || code[2] == 0xBC);
```

防复发的教训,比 bug 本身更值钱:任何基于机器码字节模式的检查,都必须**枚举所有等价编码**。x86-64 的 `mov` 立即数加载就有两种、assembler 还会按立即数范围自动挑最短的——链接地址哪怕动一个字节(而 BSS 段大小的任何变化都会动链接地址),都可能让 assembler 换一种编码,把你那套只覆盖一种情况的字节检查直接打穿。更稳的做法是不依赖具体编码、改成校验入口结构(比如反汇编一条 `cli` + 一条写到 `rsp` 的 `mov`),但在 mini kernel 这种连反汇编器都没有的早期环境里,「枚举等价编码」是最务实的一档。
