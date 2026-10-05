---
title: 02 · 零构造世界的注册簿
description: "静态构造在内核里是死路:没有 C++ 运行时,构造函数没人调用。TEST() 把用例记录摆进 .test_cases 链接段,链接脚本立围栏,KEEP 保命,发货件里段空着、一分钱不花。每件用例打一行 [RUN] 再执行,死机时串口最后一行自报家门。围栏对齐差一档,8 字节的缝被当成用例跳了进去——实弹教训整段复盘。"
chapter: 9
order: 2
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - testing
---

# 零构造世界的注册簿

注册的路,host 那半边走得很顺:TEST() 宏造出一个 Registrar 的静态对象,它的构造函数赶在 main 之前把用例登记进表。咱们把同样的套路往内核里一搬,当场就走不通了。内核是零全局构造的世界,上一卷立过的零构造纪律在这儿管着:没有 C++ 运行时,静态对象的构造函数没人调用。该跑的构造函数不跑,登记就没了下文,表是空的,runner 走一圈什么也碰不着。main 之前干活的路,在内核里从头到尾就是死的。

路是死的,活儿却是要办的。咱们换一条不靠任何代码跑的路:让用例记录在链接的时候就各就各位。TEST() 宏在内核世界里的真身是这么两笔:

```cpp
    __attribute__((used, section(".test_cases"))) static const ::cinux::test::TestCase             \
        CINUX_TEST_CAT(cinux_test_case_, __LINE__){case_name,                                      \
                                                   CINUX_TEST_CAT(cinux_test_fn_, __LINE__)};      \
```

一个 static const 的 TestCase,带着 used 和 section 的两样属性,躺在每个用例自己的编译单元里。section 把它送进名叫 .test_cases 的输入段——所有用例的记录,链接的时候会被归拢到一处。used 防的是摘除:编译器要是觉得哪个变量没人引用,是会悄悄把它扔了的,而测试用例恰恰个个“没人引用”——除了 runner 没谁叫它们的名字,咱们不护着点,链接器一伸手它们就没了。

记录归拢的地界,咱们得画出来。链接脚本 `kernel/arch/x86_64/kernel.ld` 的 .data 段里立着两根围栏:

```
.data : ALIGN(16) {
    *(.data*)
    . = ALIGN(16);
    g_test_start = .;
    KEEP(*(.test_cases))
    g_test_end = .;
}
```

KEEP 和 used 打成的双保险,gc-sections 再怎么扫也不动段里一个字节。围栏折进 .data 的尾巴,还有一层附带的好处:发货的内核不带任何用例,输入段是空的,两根围栏落在同一个地址上,一个字节都省下了。咱们拿 nm 翻发货内核的符号表,测试的痕迹真的只剩 g_test_start 和 g_test_end 一对名字,地址一模一样。测试走的是另一个产品,所以发货件不为它花一个字节。

runner 怎么走,您看 framework_kernel.cpp 的主循环就明白了:

```cpp
for (unsigned long index = 0; index < kCount; ++index) {
    const TestCase& current_case = g_test_start[index];
    cinux::print::Println("[RUN] %s", current_case.name);
    if (run_single(current_case)) {
        cinux::print::Println("[PASS] %s", current_case.name);
    } else {
        cinux::print::Println("[FAIL] %s", current_case.name);
        ++failed;
    }
}
```

进门的第一件事是打一行 [RUN],然后才执行——次序是反不得的。为什么讲究这个:哪件用例要是把机器直接带走了,串口上的最后一行就是它的名字,搜尸的时候一眼锁定凶手。咱们管它叫验尸行。判定过没过用的是快照:run_single 进门前记下 g_failures,出门的时候没涨才算过——不看返回值,返回值的事谁也没约定过,数失败次数才是两个世界通用的硬凭据。

注册簿咱们是放过一炮实弹才焐热的,所以值得整段复盘。TestCase 的身子是一双指针,x86-64 上足足十六个字节的身长。手册上 ABI 对这样的聚合其实只保到 8 字节对齐,真给到 16 的,是 GCC 搬运 16 字节聚合时的过对齐——判据咱们不猜手册,看链接器排出来的段对齐,它给的就是 16。起初咱们给围栏只写了 ALIGN(8):.data 的尾巴要是恰好停在只对齐到 8 的地址上,链接器就得在围栏与头一件用例之间垫一条 8 字节的缝。runner 是按地址硬走的,缝被它当成了头一件用例——名字读出来是垃圾,函数指针指着名字字符串自个儿的地址,跳进去执行字符串的内容,页错误当场就来了。最气人的是手动跑过一回还是过的:那一回纯属运气,段的落点恰好对齐了。落点的运气是最会骗人的毛病——多一行代码、换一个文件,它就翻了脸,红绿从此不归咱们管。修法倒是只有一行,围栏升到了 16,缝从根上没了。教训咱们记下:数据有对齐的要求,围栏的规格就得比它更严——宁可高不可低。

宏的里头还有一个小机关,顺带着看:名字拼的是 `__LINE__`,而不是 `__COUNTER__`。计数器的毛病是一次宏展开里会走三步,函数、记录、声明各拿各的数,拼出来三个对不上的名字。行号在一次展开内是稳的,函数和记录拼的是同一个名字,两边才配成了一对。这一手咱们翻符号表就能验货:记录名字的尾巴上,带着的就是用例所在的行号。

注册簿有了,runner 也会走了,串口上一行一行地报。可测试跑完了,机器照旧停进了 Halt 里睡死——红绿只写在了串口上,谁来判?下一节,咱们给开机找一扇打完就退场的门。
