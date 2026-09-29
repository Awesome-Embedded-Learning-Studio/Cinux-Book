---
title: 07 · boot 血统的第一件测试
description: "编译期能查的全部升成 static_assert,测试文件只留下真要跑起来的东西。boot 的头文件头一回进测试流水线。"
chapter: 2
order: 7
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - testing
  - static-assert
---

# boot 血统的第一件测试

收工之前咱们回一趟您开发用的电脑。`ctest` 的清单里,本站多了一件新出品:`test_e820`。boot 目录里的东西头一回进了测试流水线。有意思的是,您把它打开会发现它短得有点过分:两个用例、二十来行,这么短是因为活儿被挪去了编译期。

## 编译期能查的,全升 static_assert

这个测试的头一版,咱们老老实实写了一堆用例。有查 sizeof 是不是 24 的,有查 offsetof 对不对的,还有查 ClassifyEntry 各个值映射成什么,和 IsUsable 给不给准话的。写着写着咱们心里发虚:这些东西里,有一样是跑起来才知道的吗?一样都没有!sizeof 和 offsetof 是纯粹的常量表达式,ClassifyEntry 和 IsUsable 是 constexpr 的函数,这些用例查的全是编译那一刻就已经知道答案的事实。运行时测试跑编译期已知的事,等于叫考官拿着答案监考自己的卷子,考得再怎么热闹,卷子上也没什么新字。

所以定稿的方向,是咱们把它们全体升格:凡编译期可判定的统统写成 static_assert,住进了 `e820.hpp`,长在声明的旁边。上一节已经把它们尽数交给了头文件:布局、存档总大小、映射,咱们一类都没落下。

## 留下来的两个用例

```cpp
TEST("e820: constexpr mapping agrees at runtime") {
    ASSERT_TRUE(ClassifyEntry(1) == EntryType::kUsable);
    ASSERT_TRUE(ClassifyEntry(0) == EntryType::kReserved);
    ASSERT_TRUE(ClassifyEntry(0xFFFFFFFFU) == EntryType::kReserved);
    ASSERT_TRUE(ClassifyEntry(5) == EntryType::kBad);
}

TEST("e820: memory map is host-constructible") {
    cinux::boot::MemoryMap map{};
    ASSERT_EQ(map.count, 0U);
    map.entries[0].base   = 0x100000;
    map.entries[0].length = 0x1000;
    map.entries[0].type   = 1;
    ASSERT_EQ(ClassifyEntry(map.entries[0].type), EntryType::kUsable);
}
```

第一个用例盯的是两条求值路径。static_assert 走的是编译器常量求值的路,可 `ClassifyEntry` 真被调用的时候,走的又是代码生成的路。两条路该给出一样的答案,不过那终究是理论上的事。咱们让它在运行时再答一遍,ctest 给咱们的绿灯,就是两边答案在真跑起来的代码里也一致的记录。

第二个用例盯的是存档类型在 host 世界能不能用。您看它查的几样:构造了一回,`count` 从零起了步,条目写了一条,裸 type 也被 ClassifyEntry 消化了。这里有个细节咱们得讲明白:`MemoryMap map{}` 的 `count` 归零,靠的是 host 世界里语言保证的值初始化,恰是 boot 的 `.bss` 没有的待遇。两个世界在这里各走各的路:boot 侧没人假设它是零,host 侧编译器替咱们盯着零。眼下这个用例干的事情还很少,它的价值在将来,host 那边哪天要添上消费图谱的逻辑,起跑的位置已经踩在这儿了。

其实还有一层检查,咱们一行断言都不用写,靠的就是 `test_e820.cpp` 这个文件本身的存在:只要它 include 了 `e820.hpp`,host 的 -m64 世界就被拉来把这个头再编一遍,断言在 LP64 语境下又过了一遍。双世界各站一班岗是怎么回事,上一节咱们已经亲眼验过了。少了这个翻译单元,那套断言就只剩 boot 一家在编了。

## 一份声明,两个世界

base 库那边的断言是另一种安排:同一份声明,宿主和内核各备一组自己的实现,接进构建靠的是链接期的选择——这边连实现都不用备,头文件一 include 就完事了。真正要接的是管线:咱们在测试的 include 路径里添上仓库根,好让 `#include "boot/e820/e820.hpp"` 找得到自己的家。`add_cinux_test(e820)` 照旧一行登记了事,ctest 清单里从此有了它。将来 boot 里再添进别的纯逻辑,咱们让它顺着同一条路进场就行。

## 测不了的,坦白

咱们有两样东西,在 host 上是天生测不着的,咱们干脆不装能摸着。头一样是 BIOS 的协议行为,CF 怎么拉、SMAP 写没写回、令牌怎么续,只有咱们在 QEMU 里端到端跑一遍,才轮得到咱们验它。连着 asm 边界的正确性也一样,那个漏绑 EDX 的旧事您还记得吧:编译器到死都不知道暗号该进 EDX。第二样是 `CollectMemoryMap` 的控制流,它的截断、收工、中途翻脸,咱们在 host 上没法真的走到,因为这个循环住的是动作件,host 上没有什么 BIOS 可调。咱们真要测,得把迭代和调用的两部分分开,再注入一个假的 BIOS。眼下除了 boot 自己,图谱还没有第二个读者,没有消费者的测试架子,咱们现在不搭,等将来真有要吃图谱的代码,咱们再议不迟。

末了笔者还想啰嗦一句,提醒您,也提醒笔者自己:别因为测试文件短,您就手痒往里加用例。咱们是刻意只让这个文件留运行时才答得出的断言的,别的全升去了编译期。谁再想往这儿塞一条 sizeof 之类的检查,咱们劝他歇手:那是把升格的活儿往回搬。
