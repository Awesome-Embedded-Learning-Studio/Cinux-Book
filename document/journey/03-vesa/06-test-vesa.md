---
title: 06 · boot 血统的第二件测试
description: "能升编译期的全升 static_assert,测试文件只留两个哨兵;MakeSample 工厂为什么收一个非 packed 的样本结构体,test_vesa 首链又欠过谁一个 main。"
chapter: 3
order: 6
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - testing
  - static-assert
---

# boot 血统的第二件测试

照着上一站立好的路数,咱们收工之前回一趟 host。`ctest` 的清单里,boot 出品添到了第二件:`test_vesa`。它的身板跟 `test_e820` 是一个模子,都短得有点过分了。这么短是因为编译期能查的又一次全体升了格,都住进了 `vesa.hpp` 的 static_assert:两份缓冲的尺寸与偏移、存档的 17 字节、`MatchesRequest` 的正反例,咱们全放在头文件里长住。测试文件里剩下的,只有运行时才答得出的东西。

## 两个哨兵

```cpp
TEST("vesa: constexpr matching agrees at runtime") {
    auto const kLfb32 = MakeSample({.attributes  = 0x0080,
                                    .pitch       = 4096,
                                    .width       = 1024,
                                    .height      = 768,
                                    .bpp         = 32,
                                    .framebuffer = 0xFD000000});
    ASSERT_TRUE(MatchesRequest(kLfb32, 1024, 768, 32));
    ASSERT_TRUE(!MatchesRequest(kLfb32, 1024, 768, 24));
    ASSERT_TRUE(!MatchesRequest(MakeSample({.attributes  = 0x000A,
                                            .pitch       = 3072,
                                            .width       = 1024,
                                            .height      = 768,
                                            .bpp         = 24,
                                            .framebuffer = 0xFD000000}),
                                1024, 768, 24));
    ASSERT_TRUE(!MatchesRequest(MakeSample({.attributes  = 0x0090,
                                            .pitch       = 3072,
                                            .width       = 1024,
                                            .height      = 768,
                                            .bpp         = 24,
                                            .framebuffer = 0}),
                                1024, 768, 24));
}
```

哨兵一盯的还是求值的两条路径:static_assert 走常量求值的路,运行时的调用走代码生成的路,两边理应给咱们同一个答案。头一条比对的样本属性位、宽高、色深全对上,第二条拿 24 位的色深去问同一个 32 位的样本,这一次就该被咱们挡下了。剩下的样本各缺一样:一个的属性位不是 0x0080,另一个的 framebuffer 写的是零,咱们的 `MatchesRequest` 对它们两个都得摇头。

第二个哨兵考的是另一件事:咱们构造一整套存档和两份缓冲,看它们在 LP64 的 host 世界里到底活不活得下去:

```cpp
TEST("vesa: framebuffer archive is host-constructible") {
    cinux::boot::FrameBufferInfo archive{};
    archive.physical = 0xFD000000ULL;
    archive.pitch    = 4096;
    archive.width    = 1024;
    archive.height   = 768;
    archive.bpp      = 32;
    ASSERT_TRUE(archive.width == 1024);
    ASSERT_TRUE(archive.bpp == 32);

    cinux::boot::VbeInfoBlock  ctrl{};
    cinux::boot::ModeInfoBlock mode{};
    ctrl.version = 0x0300;
    mode.width   = 1024;
    ASSERT_TRUE(ctrl.version == 0x0300);
    ASSERT_TRUE(MatchesRequest(mode, 1024, 768, 32) == false);
}
```

您看它末尾那一句:一个只填了宽度的 `ModeInfoBlock`,属性位还都是零的,`MatchesRequest` 当场就该给咱们 false。`FrameBufferInfo` 的 17 字节,靠的也是这个翻译单元的存在,在 64 位的 ABI 语境里又闸了一道。

咱们还有一层检查,一行断言都不用写:测试文件 include 了 `vesa.hpp`,host 的 -m64 世界就被拉来把整个头再编一遍。上一节讲对齐漂移的时候,咱们已经见过同一套断言抓出来的东西,这里就不重复了。

## MakeSample:为什么多一个结构体

您可能留意到头文件里多了个 `ModeSample`,它是个不 packed 的六字段小结构,配了一个 constexpr 的 `MakeSample` 工厂。它们伺候的是谁?是 static_assert 的样例,也是您眼前测试里的同一条。

最早的写法很朴素:给 `MakeSample` 递六个散参数。可其中四个数的类型是 `unsigned short`,宽、高、色深要是给传反了,编译器就一声都不吭了。上一站咱们把栈的落点收拢成 `BootStack`、把区间收拢成 `MemoryRegion`,防的就是同型参数互换的手。这里咱们照方抓药,样本也收拢成了结构体,调用点上咱们用指定初始化器把字段逐个写明白了,`.width = 1024` 明晃晃写在咱们眼前,想传反都没有落笔的位置。

`MakeSample` 工厂函数里还有一个细节值得咱们说。咱们本来想让指定初始化器直接作用于 `ModeInfoBlock`,可 GCC 的 `-Wextra` 连指定初始化器跳过数组字段都要咬,而咱们的构建把这类告警升成了错误。所以 `MakeSample` 换了个姿势:值初始化 `{}` 把整块缓冲归了零,咱们再逐字段赋值。附带的好处是样本里忘了填哪个字段了,漏掉的那个就停在零上,不会被当成填过的值带进去。

测试第一次链接的时候,链接器给咱们递回来一个 `undefined reference to main`。咱们盯着报错愣了半秒才想起来,框架的 `RunAll` 要靠测试文件自带的 `int main`,`test_e820` 尾巴上的那一行,咱们复制过来就绿了。第二件 boot 测试抄第一件的作业,连漏抄哪一行都一模一样了。

## 测不到的,照旧坦白

咱们跟上一站一样不装。BIOS 的协议行为,写了签名没有、AX 给的是不是 0x004F,这些事在 host 上都没有 BIOS 可供咱们问,咱们只有到 QEMU 里端到端地跑上一整遍,才算真的验过了。问价循环的控制流也住在动作件里,host 这边是够不着的。咱们能交给编译期和 host 的,只有判定逻辑和结构布局这两块纯逻辑了。
