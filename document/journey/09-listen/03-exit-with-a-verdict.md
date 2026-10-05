---
title: 03 · 打完就退场
description: "isa-debug-exit:往 0xF4 写一个字节,QEMU 进程自己退,退出码 (V<<1)+1。退出码的解码表、CTest 三层、注册表的名字与层、初始化阶梯——模块测试的世界,等于该模块出生之前的世界。塞一件必败用例亲眼看它抓红,外加会疼的自证三件:sgdt、sidt、ud2 布防可恢复。"
chapter: 9
order: 3
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - testing
  - cmake
---

# 打完就退场

测试跑完了,机器停在了 Halt 里睡死,红绿写在串口上——可外面守着的构建,怎么知道该判红还是判过?最土的一条路是盯串口哨兵加超时:慢,而且全过和挂死都得等满超时才分得开。咱们没挑它,选了 isa-debug-exit,QEMU 专给测试备下的出口设备。协议其实就一句话:往 0xF4 端口写一个字节 V,QEMU 进程就自己退了出去,退出码给的是 (V<<1)+1。机器不用死等——成绩单是当场交的。

框架那头的写法,咱们原样摆出来:

```cpp
[[noreturn]] void exit_qemu(unsigned char code) {
    cinux::driver::OutB(cinux::driver::PortWrite{.port = kQemuDebugExitPort, .value = code});
    cinux::arch::Halt();
}
```

调用它的时候,code 递的是 failed + 1。咱们一道一道算:全部通过是 0 失败,写进去的是 1,QEMU 退的是 3。坏了一件,写进去的是 2,QEMU 退的是 5。为什么 failed 要加一?写 0 的话全过就成了退出码 1,而退出码 1 是“根本没跑到出口”的保留号,再好的成绩也不能冒领。这儿还埋着一处容易抢戏的地方,咱们差一脚就踩进去:写端口之前,替 QEMU 把 (V<<1)+1 的结果提前算好再写。全过本该退 3 的,退成了 7。协议说的是“写 V,设备退 (V<<1)+1”,编码是设备的活,咱们抢下来,设备就在外头又编码了一层,数字全乱了。您跟芯片打交道,谁该干的活就归谁。

外面接成绩单的,是 test/ 那头一层 CTest 的包装,判分的章程,咱们全写在 `cmake/test/run_kernel_tests.cmake` 里。退出码 3 报的是全过。比 3 大的,拿 (码-3)/2 算出坏了几件。1 的意思是内核根本没走到出口,早死了。剩下的,要么挂死了,要么就是崩溃了。内层还给了一次六十秒的超时兜底——真挂死了,QEMU 的进程也活不过六十秒。判红了怎么办?脚本把串口日志整份打出来再失败:哪件用例、哪个断言、哪一处,全都在里头了。上一节的验尸行,在这儿派上了正经用场——验尸报告是自动附卷的。

三层楼怎么分工,咱们从上往下数。顶层的 CMakeLists 持有两样公共家当:头一样是 QEMU 的设备参数,-accel kvm 加 isa-debug-exit 的组合,run 目标和测试用的是同一份。另一样是咱们原样抄来的注册表,倒是一共就两行:

```cmake
set(CINUX_KTEST_STAGES exceptions irq)
set(CINUX_KTESTS selfcheck:exceptions tick:irq)
```

一条记的是两个词:名字说它是谁,层说它的内核开机要走到哪一级。层的写法得在层词表里对得上号:写错了一个,配置期当场就把错报了,不带病地生成。一条的代价也是实打实的:一个测试文件、一个内核、一个镜像、一次 QEMU 开机、一个 CTest 条目,一件都省不了——每件测试住自己的文件、开自己的机,暗排次序的机会是没有的。听着是重了些,可一次开机就是零点几秒的事,咱们出得起。中间那层 kernel/ 出的是两个函数,add_cinux_kernel 造发货的内核,add_cinux_ktest 造测试的内核。底下的 test/ 把 CTest 条目包了起来,再立的就是 test_host 和 test_kernel 两个聚合目标,一个只收桌面的,一个只收机器里的。三层各拿各的家当,谁也不越谁的地界。

层背后是一条更深的道理,咱们叫它初始化阶梯。test_main.cpp 里台阶是用宏分的:

```cpp
#ifdef KTEST_STAGE_IRQ
    cinux::arch::Pic::self().remap();
    cinux::time::Tick::self().init(cinux::driver::Pit::self());
    cinux::arch::irq::InstallIrqStubs();
    cinux::interrupt::Irq::self().enable_line(cinux::interrupt::IrqLine{.value = 0});
    asm volatile("sti" : : : "memory");
#endif
    cinux::test::RunKernelTests();
```

块尾的那声 sti,是把 CPU 的中断总闸拉开的那一声,它的讲究,咱们留到开闸的那一章细说。测异常层的内核,开机只走到异常层就停了。测中断层的,才把中断的五步全带上。为什么这么抠:模块测试的世界,等于该模块出生之前的世界。您测的是异常层,机器的内核里却跑着中断,异常一旦红了,您分不清是异常自己的错,还是中断进来搅的浑——灶台单独搬出来验,就别让别的火还开着。至于几个模块真协作起来的综合内核,那是另一张清单上的事,等第一个真协作出现了再说,笔者的预感是调度器,到时候它自己会来讨的。

顺着阶梯咱们还立了几条防偶然绿的章程,挑实在的说。头一条讲的是每件测试住自己的文件:两件用例要是暗地里排了次序,一件的副作用喂了另一件的前提,红绿就听天由命——各住各的文件,各开各的机,谁也别沾谁的前件。另一条是时序的断言只做单边下界,外带自旋的上限防挂死。等得到的用例会过,等不到的会红——但不许把整次开机拖到超时。末一条是关过中断的区段,自己把 sti 收了回来,要不然后面所有的用例全在静默里陪葬。这几条眼下听着挺抽象的,等本卷尾巴上内核侧的 tick 测试出场,一条一条都会对上号的。

框架自己说能抓红的话不算数,咱们得亲眼看过它抓红。于是有了红路实弹:临时往 selfcheck 里塞一件必败的用例,重跑了一回。CTest 当场就红了,QEMU 退的是 5,按 (5-3)/2 的算式一解码,坏的是一件。串口上有它的名字、断言的原文和位置,Results 一行报的是 3 passed, 1 failed——后头的用例照跑不误伤,一件红了不连坐。验完把必败的撤干净,复跑了,退出码回到了 3。红路走过了一回,绿才算有了分量。

阶梯的第一级,住的是 selfcheck,咱们管它叫会疼的自证——上一卷立的那些家当,如今由机器肚子里的指令自己作证。头一件咱们问描述表:

```cpp
    cinux::arch::gdt::TablePointer gdtr{};
    asm volatile("sgdt %0" : "=m"(gdtr) : : "memory");
    ASSERT_EQ(gdtr.limit, sizeof(cinux::arch::gdt::KernelGdt) - 1);
```

sgdt 是 CPU 自家的指令,把肚子里正用着的描述表地址和身长报出来。limit 应该等于三槽表的字节数减一——上一卷咱们亲手立的表,现在由使用它的人出面作证,顺手还拿 movw 验了代码段的选择子,0x08 分毫不差地对上了。第二件咱们问中断表,sidt 报出来的 limit 应该是 4095:256 个门,每门的 16 字节、末了减一。第三件才是最妙的,咱们请出 ud2:

```cpp
    ASSERT_TRUE(cinux::arch::isr::ArmRecoverableFault(kVectorInvalidOpcode, kUd2Length));
    asm volatile("ud2");
    ASSERT_EQ(cinux::arch::isr::TakeRecoveredVector(), kVectorInvalidOpcode);
```

布下了防,然后当真犯一个非法的指令。异常还是来了,可这一回 ReportFault 进门头一件事是查布防:命中了,就把现场里的 rip 往前挪两个字节,iret 回来了——不打印、不停机,ud2 是两字节长的指令,挪过去正好落在了它身后,程序也就接着跑了。读一次就消费掉了,第二发 ud2 没有布防的话就是真死。您看,这是真内核异常表的微缩版:大内核里,拿异常去试探一段不许碰的访问,修复点把它接住了,是成套的机关。咱们这儿把同一路手艺缩成了三行。恢复的路径纯寄存器、不碰打印:iret 跳回来之后,向量寄存器不能是脏的。上一卷 SSE 开禁时埋下的隐藏条款,在这几行里兑了现。

异常层的证词收齐了,阶梯的第一级站稳了。可本卷的正主还没出场:中断表上,32 号之后的十六个座位还空着。下一节咱们去请门房。
