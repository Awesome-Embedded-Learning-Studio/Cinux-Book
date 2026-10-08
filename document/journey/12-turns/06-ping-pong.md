---
title: 06 · 乒乓
description: "两任务三轮乒乓、退出收割、回主流程,串口六行是这一幕的剧本。机内三案把切换按住验:交替序、四个哨兵加 xmm0 跨切换存活、栈底魔数、八轮生灭页数回平。种哨兵首版闪断的公案,教的是内联汇编的输入保持约定和加一行打印就好转的鉴别法。"
chapter: 12
order: 6
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - process
  - testing
---

# 乒乓

零件全齐了:会拍照的汇编、会造任务的 builder,会决策的调度器,真动手的 sink。本节咱们把它们组装起来,放一场最朴素的多线程戏码——协作式乒乓。协作两个字的意思,是换手全凭自愿:任务自己喊了 yield,咱们这才给它换人。谁也不会被咱们强行打断。它是三幕里最老实的一幕,也是地基:后面的抢占和睡眠,都是在这套换手机制上换的触发方式。

演示的代码咱们原样摆,组合根里就这么几行:

```cpp
auto cooperative_echo = +[]() {
    auto& scheduler = cinux::proc::Scheduler::self();
    for (unsigned int round = 0; round < 3; ++round) {
        Println("[task] tid=%u '%s' round %u", scheduler.current()->tid,
                scheduler.current()->name, round);
        scheduler.yield();
    }
};
cinux::proc::InstallKernelSwitchSink();
auto* const kEchoA =
    cinux::proc::TaskBuilder{}.set_entry(cooperative_echo).set_name("echo_a").build();
auto* const kEchoB =
    cinux::proc::TaskBuilder{}.set_entry(cooperative_echo).set_name("echo_b").build();
cinux::base::safety::Check(kEchoA != nullptr && kEchoB != nullptr, "echo tasks failed to build");
cinux::proc::Scheduler::self().seat(*kEchoA);
cinux::proc::Scheduler::self().seat(*kEchoB);
cinux::proc::Scheduler::self().run_until_done();
Println("[kern] cooperative tasks drained, back on main");
```

咱们只写一个入口函数,两个任务共用:打印自己的 tid 和名字,喊了声 yield,三轮之后就自然返回了,从蹦床走进统一的退场。机器跑下来的串口,咱们原样抄:

```text
[task] tid=1 'echo_a' round 0
[task] tid=2 'echo_b' round 0
[task] tid=1 'echo_a' round 1
[task] tid=2 'echo_b' round 1
[task] tid=1 'echo_a' round 2
[task] tid=2 'echo_b' round 2
[kern] cooperative tasks drained, back on main
```

一轮不多一轮不少的六行交替,末了那句 back on main 是主流程的声音——任务全退光了,run_until_done 把控制权还了回去,落点正是 kernel Main 的栈。您体会一下这七行字的分量:打头那行是内核出生以来头一次,有别的执行流在串口上说话。末了那行证明照片机制是完整的——走的时候存了照,回来的时候对得上,主流程的世界一丝没乱。中间的六声 yield 加上两次退场换手,每一声都穿过了关中断、回队、切换、收割的这一整条链。

演示看过了,咱们照例把验收做严。机内的 sched 台,这一幕进了三案,验收落在了四个点上。头一案验的是次序:两个任务把三轮乒乓打满,记录下的 tid 序列必须严格交替,而且相邻两个绝不相同——它断言的,是 FIFO 队列与 yield 语义合起来的秩序。第二案是最硬的,咱们单独说:寄存器哨兵。切换代码声称保住的是八个 callee-saved 槽外加 SIMD 现场,这样的声称空口无凭,咱们派哨兵去查。咱们给任务写一段内联汇编,把 r12 到 r15 各装了一个魔数、xmm0 装了一个,然后喊了 yield 让别人跑一圈,回来再把寄存器的值读出来对——咱们手里是四个加一个,五个哨兵哪个变了,变的就是没保住的那一槽。同一案顺带验的还有栈底魔数:任务跑完了全程,栈最底下那个字还得是出厂的 0xDEADC0DE。第三案验的是回收:循环八次,造一个进去就退的短命任务,跑完了八轮,名册的空闲页数必须与起跑前一页不差。生灭了八轮,咱们清点下来,栈和对象两样的家当一件行李都没丢。放出去的资源还得收得回来,这是本卷对每一批新件的统一要求。

哨兵的那一案,咱们还得交代一段公案,因为它的首版闪断过,而闪断的方式太典型。首版的写法是把种哨兵和读哨兵放在同一个 asm 块里,yield 夹在中间——逻辑上是无缝的,断言偏偏红得没有道理,更邪门的是加一行打印它就绿。咱们把反汇编和 gdb 都请出来才看清真相:asm 块里夹着的 call(yield 是函数调用)会破坏 caller-saved 寄存器,可 GCC 跟 asm 块的约定,是输入操作数跨块保持——编译器压根不知道块里的 call 会打坏东西,所以块一结束就敢拿输入寄存器当完好数据用。哨兵值恰好被分配到了 rax 上,rax 被 call 搅成了一团垃圾,断言念的全是垃圾,真假全凭的是运气,这就是所谓的闪断。修法是让形状替咱们把关:两个纯 asm 块,中间夹一个普通的 C++ 调用,种和读各自成了块,编译器跟两个块的约定各自成立,从此就稳定了。这一案咱们记下两条:跟内联汇编打交道,守的就是块里不夹 call 的纪律。而加一行打印就转绿的症状,是布局敏感的典型病征,您也别高兴,去把该查的约定查清楚。

长凳的那一头,test_context 四案验的是照片本身(布局断言、蹦床指向、魔数落点、出厂浮点字),test_scheduler 三案验决策流(发号、FIFO、yield 交替到退场回收的六步序列),上一节的回收泄漏就是在这儿现的形。地基这一幕到此收工。可协作式的死穴咱们也摆在桌面上:yield 是君子协定,来了一个不喊 yield 的任务,全机就归它了。下一节咱们把时钟请出来,让它替不想让的人做主。
