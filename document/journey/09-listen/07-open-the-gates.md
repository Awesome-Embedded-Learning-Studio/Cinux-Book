---
title: 07 · 开闸
description: "remap、启动心跳、装桩、开 0 号线、sti——五步的次序一步不能乱,理由逐条给。芯片线闸加 CPU 总闸,与进保护模式那年的关中断纪律首尾对影。Halt 搬进 arch,hlt 从睡死翻成睡着等叫醒。内核侧两件测试:下界三拍,关门静默。"
chapter: 9
order: 7
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - interrupt
---

# 开闸

桌椅都齐了,咱们看组合根里的五步。kernel.cpp 的 Main 收尾,原样地摆出来:

```cpp
    cinux::arch::Pic::self().remap();
    cinux::time::Tick::self().init(cinux::driver::Pit::self());
    cinux::arch::irq::InstallIrqStubs();
    cinux::interrupt::Irq::self().enable_line(cinux::interrupt::IrqLine{.value = 0});
    asm volatile("sti" : : : "memory");
```

五行的次序一步不能乱,咱们一步一步给理由。remap 排在头一行:向量的挪位必须在开门之前完成,而排在头一行,是最不容易踩错的落法。设备早启动无害:Pit 在第二行就敲起了拍子,可芯片的线闸全捂着,敲了也白敲,等真开门的那天它已经在拍上了。装桩得赶在开门之前:要是门开了而桩还没装,32 号之后立着的还是一排零门,头一响落的就是一般保护异常,行话里的名字是 #GP——CPU 查门的时候连类型都认不出,当场就翻了脸。转储是打得出来的,可您得对着一场莫名其妙的转储猜半天。开 0 号线得赶在总闸之前:总闸一开的话,芯片那头的门就得已经管好了,不能开了总闸再手忙脚乱去开线闸。压轴的是 sti,它是 CPU 自家的总闸。

您把视野拉远一点看,这儿其实有两道闸。一道是芯片上的线闸,unmask 一条一条地开。另一道是 CPU 里的总闸,认的是 IF 标志——sti 拉起、cli 落下。两道闸里少了任何一道,中断都到不了内核。线闸关着的时候,门房收了声,是不往上递的。而总闸要是关着,门房喊破了嗓子,CPU 也是不理的。您还记得进保护模式那一卷立的关中断纪律吗?cli 从那年一路关到今天,为的是换桌椅的时候没人进来踩了脚。今天桌椅摆齐了,咱们把它收回来。当年关的是门,今天开的是闸,守的是同一句担心,办的是同一件体面。

总闸一开——面板就亮起了第六行,咱们原样抄来:

```text
[kern] irq on, tick 100Hz
```

打的是 kTickHz.value——咱们在旋钮上定的数——面板照实转述。从那儿起内核才算真的会听了。

趁着收尾咱们还办了一件搬家的小事。Halt 从 console 家搬了出来,新家安在了 kernel/arch/x86_64/halt.hpp。为什么该搬:停车指令 hlt 属于 CPU,而不属于控制台,console 一个管说话的,凭什么替机器管睡觉。更要紧的是语义翻了身。关着中断的 hlt 是睡死——没人来叫,一觉也醒不了,咱们前头那些卷的收尾全是它。开着中断的 hlt 是睡着等叫醒——下一个中断一到,CPU 醒了过来,跳进表里的桩,办完了事接着睡。起局的收尾从“关灯等死”变成了“睡着听世界”,同样的一行 asm,本卷前后过的是两种人生。

说得这么热闹,谁来验货?内核侧的 test_tick,咱们两件都看:

```cpp
TEST("tick: heartbeats arrive once the gates are open") {
    const unsigned long long kBefore = cinux::time::Tick::self().since_boot();
    unsigned long long       spins   = 0;
    while (cinux::time::Tick::self().since_boot() < kBefore + kHeartbeatsWanted &&
           spins < kWaitSpinCap) {
        ++spins;
    }
    ASSERT_GE(cinux::time::Tick::self().since_boot(), kBefore + kHeartbeatsWanted);
}
```

心跳到达件的写法:记下起点、自旋等计数涨过三,自旋的上限二十亿防挂死,等得到的会过,等不到的会红,机器是不会被它拖到超时的。断言走的只是下界,您别嫌它松:KVM 底下是实时的钟,自旋多久、赶上的是一拍还是两拍,本来就是不确定的。真要确定性的话得上软件模拟,又跟咱们只跑 KVM 的红线冲突,所以不做。下界说明问题已经够用了:涨过三拍,就证明线通着、门开着、应答也忘不了——三拍连着到,缺了哪一环都断流。

```cpp
TEST("tick: silence while the cpu gates are closed") {
    asm volatile("cli" : : : "memory");
    const unsigned long long kFrozen = cinux::time::Tick::self().since_boot();
    for (unsigned long long spins = 0; spins < kQuietSpinCap; ++spins) {
    }
    ASSERT_EQ(cinux::time::Tick::self().since_boot(), kFrozen);
    asm volatile("sti" : : : "memory");
}
```

关门静默件的章程:cli 之后计数应该冻结。一亿次的自旋下去、一拍不涨,断言的是两边相等。跑完咱们自己把 sti 收回来。防偶然绿的末一条在这儿也兑了现:关过中断的区段自己收对,少了这一下,后面的用例全在静默里陪葬。

咱们还真跑过一回一次性的实验。把 dispatch 里的应答注掉,再开机:心跳只来一拍——芯片把 0 号线锁了,第二拍永远到不了,下界断言当场就红了。应答的纪律写在纸上是提醒,写进测试里的才是岗哨。验完了就撤得干干净净,代码原样放了回去。

两道闸全开了,心跳也验过了。剩下的就是咱们对数的日子了:面板的六行,长凳的十五件,考古箱里的两段日子。
