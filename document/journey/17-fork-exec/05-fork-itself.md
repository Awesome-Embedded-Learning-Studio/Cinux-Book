---
title: "05 · fork 本体"
description: "被 96 字节帧遗漏的 r12-r15 走 GS 侧四格银行,入口汇编压帧之后、开中断之前存好。BuildForkChild 持着中断守卫克隆世界、拷一帧进新栈,ForkChildEntry 镜像返回尾,rax 强制零。帧计数升格进 PMM,每帧一字节。克隆测试恒漏一页,自查三回未中,回头再读一眼命中:frame_index 把 Floor 的返回值又除了一次页大小。"
chapter: 17
order: 5
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - process
  - fork
---

# fork 本体

引擎在前一节立好了,fork 本体剩下两块硬骨头:孩子的返回现场,和帧计数的正形。咱们从一件尴尬的旧事说起。

syscall 入口的那段汇编,压的是十二拍、九十六字节的帧,咱们在柜台那一卷逐槽数过。可您翻遍整张帧,找不到 r12 到 r15——而 Syscall 指令不碰它们,C++ 的那一侧又拿它们当 callee-saved 使,进出的两头本来都不用存。别的路径用不着存,fork 偏偏离不开它们:孩子的寄存器图像里,四个都得是用户进门那一刻的值,而父任务自己的 ctx 图像,存的是它上回被切走时的内核值,拿来也充不了数。九十六字节的帧约定,咱们一处不动,咱们的解法是给 r12 到 r15 开个小银行:per-CPU 块里添四格,等入口汇编压完帧、还没开中断的时候顺手存进去:

```asm
    movq %rsp, %gs:24
    movq %r12, %gs:32
    movq %r13, %gs:40
    movq %r14, %gs:48
    movq %r15, %gs:56
    sti
```

每次 syscall 咱们都白存一次,花的只是四个 mov,换来 fork 的随取随用。gs:24 存的是帧地址,也一并立住了。这里咱们给往后留一句:咱们两个 GS base 至今同值,swapgs 换的是相等量,配对怎么乱都乱不了——这是结构上的免疫,不是修好的病。哪天用户态真用自己的 GS 基址了,简化也免不了连着改,那天的事那天办。

咱们把 BuildForkChild 的主干端上来:

```cpp
Task* BuildForkChild(unsigned long long frame_address) {
    const arch::IrqGuard kEntryGuard;
    const auto& bank = arch::per_cpu::PerCpu::self();
    Task* const kParent = Scheduler::self().current();
    base::Own<mm::AddressSpace> space(mm::cow::CloneSharedSpace(*kParent->space));
    // —— 中间:领四页内核栈,MakeOwn 造 Task,逐字段填 ——
    kBorn->ctx = kParent->ctx;
    asm volatile("fxsave64 %0" : "=m"(kBorn->ctx.fpu) : : "memory");
    kBorn->ctx.r12 = bank.user_r12;
    // —— r13 到 r15 同路,rip 指向 ForkChildEntry ——
    kBorn->ctx.rsp = kStackVirt + kStackBytes - sizeof(syscall::SyscallFrame);
    cinux::base::CopyBytes(cinux::base::PtrAt<void>(kBorn->ctx.rsp),
                           cinux::base::PtrAt<const void>(frame_address),
                           sizeof(syscall::SyscallFrame));
    // —— fd 逐槽,cwd,name,parent,齐了交给 seat ——
}
```

入口一进就持好了中断守卫,快照从 GS 银行取好再动手——中途被时钟打断的话,守卫护着的就是这整段拷贝。ctx 整个抄的是父任务,然后改:r12 到 r15 换银行里的用户值,rip 指向的是 ForkChildEntry,rsp 落在新领的内核栈顶上,顶上摆的是父进程九十六字节帧的原样拷贝。中间的那行内联汇编值得单说:ctx 里那副浮点图像,抄来的其实是父任务上回被切走时的旧照,咱们现场补一发 fxsave64,孩子的浮点,是从 fork 这一拍起算的。

您注意到了吗,孩子只拷了一帧,没拷父进程内核栈上更深的东西。这正是设计:孩子不需要回到 fork 的 C++ 调用链里去,它的内核故事从返回尾开始。当年这个问题的另一种解法,拷整条用着的栈、让孩子从链条中间续跑,炸出了一桩名案,咱们收在这一卷的后日谈里慢慢讲。眼下的形状,是那桩名案之后的公认定式。

咱们把返回尾本人请出来看:ForkChildEntry,和 syscall 的返回段几乎是镜像:

```asm
ForkChildEntry:
    movq 32(%rsp), %rdi
    # —— 中间十拍,和返回段一样,一件一件弹回来 ——
    movq (%rsp), %rsp
    xorq %rax, %rax
    swapgs
    sysretq
```

咱们跟着孩子走一遍:头一回被切换器请上 CPU,寄存器照 ctx 的样子还原,rip 落在了 ForkChildEntry,弹帧、回用户栈、sysretq——唯一的不同是末尾那一拍,把 rax 强制清了零。父进程的返回值呢?它是 HandleFork 这个 C++ 函数的返回值,孩子的 pid,顺着 C ABI 的 rax 原路回去。您在内核这一侧看,一次调用、两次返回的样子,就是一个 rax 的两种来路。syscall 的号是 57,HandleFork 里把造完的孩子交给 seat,seat 顺手发 pid——进程那一卷的 tid 发号器,如今名正言顺地叫 pid。

帧计数的正形,这一批也立了,咱们把它升格进了 PMM:物理内存每帧一个字节,一整张的 shares_ 数组,静态铺好了。一帧被几个世界映射的时候,字节的值就是几,零是不共的人的缺省。选这个形状有出处——Linux 那边管这叫 struct page 的 refcount,咱们拿一字节的教学形态致敬原版。计数是计数的事,还帧是还帧的事:PMM 只管数,减到零之后还不还、什么时候还,是调用方的事, ReleaseFrame 替 teardown 把这句话说全了。

然后是那一页漏掉的帧。克隆测试跑了起来,断言恒差了一页,咱们自查了三回:怀疑堆生长搅局,测试里预热了一遍堆,证伪了。再看 retain 和 release 的对称,总量明明是平的,更糊涂了。案子搁下歇了口气,咱们回头再读代码,一遍就过了,病在 frame_index:把 Floor 的返回值又除了一次页大小。语义在 Doxygen 里写得明明白白的:Floor(value, unit) 返回的是 value 里装了几个 unit——页数,不是页地址。连除了两次,十六 MiB 的跨度里四千零九十六帧共用了同一个计数槽:retain 了四千次,release 了四千次,总量平得不能再平了,数却全记在了同一格上。这一案教了咱们两句:用了语义轮子,就得读它的说明书。总量的对称,证明不了逐项的正确——真正有牙的,是构造性回归:相邻的两帧,各自的计数必须互不相扰,这一案咱们当场补进了 PMM 的测试。

同一批还有四件安全上的立修,咱们报个数:cow 和 fork 两个文件进了不开向量化的那座岛,岛的名字叫 no-SSE,岛上把 SIMD 指令禁了。要在异常深度上跑向量化拷贝的话,毁的是被中断代码的 xmm,这个口子咱们不开。PMM 的 allocate_run 补上中断守卫,查找和置位之间原本留了个非原子窗口。MarkPageShared 的幂等,上一节说过了。还有 syscall 返回段的事,call 之后紧跟了一条 cli,把弹帧回家那几拍的抢占窗口收窄——等通上电您就会看到,这个窗口里能出多大的乱子。测试面:克隆镜像、单侧写分家、子的组装全检,三案落进了 test_sched。第四案是入了座的子退出进父的 wait,连同父收子尸的执行级实弹,都随收官那批落——等 shell 的 run 命令接上才算数。四条线全过了,带盘的产品链路照常到 shell 提示符。

孩子从 GS 银行里取到了自己的四个寄存器,在新栈上捧着自己的帧,从返回尾进了场。可咱们心里清楚,孩子生下来还是 shell 的镜像,得有东西把盘上的程序装进去——下一节咱们去请装载器。
