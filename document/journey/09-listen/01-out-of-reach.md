---
title: 01 · 肚子里的事,桌面够不着
description: "host 的长凳测的是能搬到桌面上的零件,内核肚子里的事一件都够不着:IDT 装没装对、sti 之后活不活着,只有开机才知道。测内核的是另一个产品——自家的 Main、同一套核心源、两个二进制,两个世界共用的凭据,是两张头文件。"
chapter: 9
order: 1
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - testing
---

# 肚子里的事,桌面够不着

咱们一路是怎么验货的,您应该有印象了:每写一件家当,咱们开发它的电脑那头就配一件测试。咱们给 host 这一头起了个小名,叫的就是长凳。上一卷咱们数下来,长凳上摆的是十四件——e820、format、framework、result、vesa、bits、gdt、layout、page、image、bitmap、pmm、kernel_image,外加描述表自家的 idt。它们的路数是一致的:把零件从内核树上摘下来,搬进咱们开发它的电脑里,喂的是假图,查的是编码,算的是取尾。您想测位图,在桌面上编一张小图给它记就行了。您想测取尾的算术,就拿真实链接出来的镜像喂它。长凳管的,就是够得着的零件。

可内核肚子里的事,长凳是够不着的。咱们拿本卷要碰的几件活来问:中断表装没装对?sgdt 和 sidt 问的是机器肚子里的寄存器,而桌面上没有机器,问谁去?sti 之后机器还活着吗?这得总闸真开了才知道。中断到底来不来?得外设真敲了门、门真开了、应答真回了,一环一环都在机器的肚子里。这些问题的共同点是:答案住在 QEMU 的肚子里,不住在咱们的桌面上。

上一卷咱们验异常,靠的是肉眼:故意 ud2 一发,盯着面板看那六件遗言报齐了没有。肉眼验了一回两回还行,可它撑不了多久——每改一行都得开机、盯串口、人脑判红绿,结果谁也熬不起。本卷要动的是更凶的活:中断一起,死法偏偏又快又哑,最坏的一种是连一句完整遗言都来不及留的三重故障。咱们得在动刀之前备好一条机器自己报成绩的路。验得了伤,而后才轮得到手术刀。

路是现成的,框架其实早就留好了门。咱们回头翻 test/framework/ 那几份头文件,会发现从最早的 host 测试起,当家的一份 test_case.hpp 就没把自己当 host 的私产——它的注释里写着,两个世界走的是同一种记录。当年第一遍也真走过:内核还很小的那天,kernel 半边的测试就真跑过一回。这一遍咱们刻意把它压着,压到今天才兑现——次序换了,路倒是没换。

兑现的头一个决定是:测内核,靠的是另一个产品。test/kernel/test_main.cpp 里有它自家的 `kernel::Main`,开机、报身份、跑用例的活,全归了它管。发货的 kernel/kernel.cpp 咱们一根手指都没碰,保持着出厂的纯净——一行测试代码都没混进来。两个内核共享的是同一份核心源,咱们在 `kernel/CMakeLists.txt` 里看得清清楚楚:

```cmake
set(CINUX_KERNEL_CORE_SOURCES
    ${CMAKE_SOURCE_DIR}/kernel/arch/x86_64/entry.cpp
    ${CMAKE_SOURCE_DIR}/kernel/arch/x86_64/exception.cpp
    ${CMAKE_SOURCE_DIR}/kernel/arch/x86_64/gdt.cpp
    ${CMAKE_SOURCE_DIR}/kernel/arch/x86_64/idt.cpp
    ...
    ${CMAKE_SOURCE_DIR}/kernel/arch/x86_64/pic.cpp
    ${CMAKE_SOURCE_DIR}/kernel/arch/x86_64/irq_stubs.cpp
    ${CMAKE_SOURCE_DIR}/kernel/interrupt/irq.cpp
    ${CMAKE_SOURCE_DIR}/kernel/lib/assert.cpp)
```

咱们拿它造两个目标:头一个,add_cinux_kernel 添上了 kernel.cpp,链出发货的内核。另一个 add_cinux_ktest 添上了 test_main.cpp 和框架,链出测试的内核。同一批 .cpp 链出两个二进制——内核的活一件不缺,测试的身份互不沾染。

两个世界共用的凭据是两张头。头一张咱们已经引过注释了,test_case.hpp 整份文件的心脏就一个结构:

```cpp
struct TestCase {
    const char* name;  ///< Human-readable case name, printed by the runner.
    void (*body)();    ///< Test body, typically a lambda emitted by TEST().
};
```

名字指针加函数指针——清一色的纯数据,一个多余的字节都没有。为什么必须纯数据,等零构造的世界把注册的路子摆出来,您就明白“纯”字省了多少事。第二张是 test_assert.hpp:g_failures 的计数、CurrentCaseName、两个 Report 钩子的声明,加上 ASSERT 的一整套宏。断言失败了往哪儿打印?声明只有一份,实现咱们一边给一份——host 的走 stderr,内核的走 COM1,到了链接期各归各。您看,console 分家那次的手法,“一份脸面,两户人家”,在这儿原样又落了一回。

宏的身子还有一处讲究,咱们得说在明处:失败的报告只带表达式的原文和位置,不带两个值的长相。为什么抠门?host 那半边的 EQ 比得动 std::string,内核那半边连转型的名字都编不出来——两个世界对不上的东西,索性两边都不带了。位置从哪儿来?source_location 的默认参数自动落在调用点上。最早立 Check 的时候用过的姿势,咱们原样搬了过来。

凭据立好了,产品也分了家。可还有一个最实在的问题咱们没答:内核的世界里 main 之前没人干活,咱们的用例从哪儿来?答案握在链接器的手里。
