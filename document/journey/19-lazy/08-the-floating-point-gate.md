---
title: "08 · 浮点的闸门"
description: "补页的代码落进树里时就记了案、收官之后才动手的一道暗缝:内核镜像里咱们今天亲手数过,列出来的五类 SSE 指令共六百一十八,零星的散指令有没有、几条,跟着编译器版本走,分布恰是环三必经之路,而 syscall、异常、中断三个入口一个都不保 SIMD 现场。补页的代码住在禁 SIMD 的岛上,经函数指针调出去的 ext2 不是岛,保证在调用缝上失效。每个任务一张五百一十二字节的进场图像,深度随任务走,内层入口自保一张内核现场,fork 改抄进场图像。这一卷新写进 /bin 的测试程序 mapper 拿十六个魔数词实弹对拍。"
chapter: 19
order: 8
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - fpu
  - concurrency
---

# 浮点的闸门

上一节的挂死案,引子是本节的主角。缝是补页代码落进树里时就记了案的,收官之后咱们才腾出手来动它。缝不在懒章程的里头,在浮点寄存器的身上。咱们把证据一件件摆出来,您自己判断这缝要不要堵。

头一件证据咱们来数个数。咱们拿反汇编把今天的内核镜像过一遍,一家一家地数:movups 占了一百八十七,movaps 占了一百七十,movdqa 占了一百三十六,movdqu 占了七十三,pxor 占了五十二,五类的和恰好凑满六百一十八,五类之外是一条 SSE 都没有的。当年交接的档案里记的也是六百一十八,只是各家的数目略有出入,还带着几条零星的散指令。零星的有无、几条,跟着编译器的版本走,两遍数下来不变的是五类凑满六百一十八这个数。这六百多条其实不算多,要命的是分布:文件世界的初始化、execve、open、ext2 的读 inode 和查找、装载器、登记册的克隆、mmap,全收在列表里了,名单列的恰好是环三请求进内核之后的必经之路。这些代码是 -O2 编出来的,编译器干活用的是 xmm,天经地义,咱们不怪它们用,咱们怪的是没人保管现场。

第二件证据:咱们查入口。syscall 的入口,您在柜台那一卷数过它的十二拍帧,通用寄存器是挨个压好的。r12 到 r15 的四格银行,是生与收那一卷为 fork 存进 GS 侧的。帧里和银行里连一条浮点指令的影子都没有。fxsave 是把一整副 xmm 现场存进内存的指令,咱们一次都没做过。异常的入口、中断的入口同样干净得彻底,也没有 fxsave 的踪影。也就是说用户程序带着一寄存器的 xmm 走进内核,内核一路跑了下去,跑多久、跑多深都碍不了事,出来的时刻,xmm 里装的是谁的值,全凭路过的代码心情。

第三件证据也是缝的精确位置。那座禁 SIMD 的岛,咱们在代码里早就立下了,生与收那一卷把写时复制和 fork 收编了进去,还给它起了 no-SSE 的名字,岛规管的是编译器不替住户发射 SSE。这一卷的补页代码也编进了岛。可补到文件页的那一步,代码拿着登记册上记的后端面,经的是函数指针,调出去读的是 ext2,ext2 倒是不在岛上。岛的保证只管自己岛内的编译单元,函数指针跳了出去,保证在调用的缝上失效。读文件的一路,进到的函数个个手里攥着几条到几十条 SSE,xmm 就地易主。

危害怎么成真,咱们拿一个具体场面说。用户程序是 -O2 编出来的,xmm 是活着的寄存器,里面存的是正经数据。它读一个还没补的文件页,缺页进了门,补页调的是 ext2,一路跑的都是 SSE,页补上了,指令回头重跑了,可它 xmm 里的数据,已经是 ext2 路上的垃圾了。咱们用不着假设,这是结构上的必然,而且是静默的损坏,而以前没炸,只是因为咱们的用户程序都还小,补页的时机没赶上 xmm 正活着的时刻。中断的缝还更宽:用户态跑着的时候中断全开着,时钟每一拍、盘的完成每一次,都可能把用户的执行打断,进内核跑了一趟 SIMD,再若无其事地回去。还有一桩连带的:fork 的现场快照,抓的是此刻的 xmm,可 fork 自己就走在 syscall 会话的深处,光是登记册的克隆就带着十三条 SSE,孩子继承的那副浮点图像,出生就是脏的。

堵法的三个候选,咱们摆开再选。头一个候选扩岛:把 ext2 和盘那一链全圈进禁 SIMD 的名单,范围就失了控,向量化的性能全丢,而且 syscall 和中断的缝还是开着,这样的修法是修不全的。只补文件读的前后,包了一对存取,可 syscall 和 IRQ 的缝同病,按住了一个葫芦浮起两个瓢。正解是统一的一道闸:每个任务带一张五百一十二字节的进场图像,环三进内核的三个入口:syscall、页故障、中断,进门把 SIMD 现场存进当前任务的图像,出门还原的还是同一份现场。轮转切换器管的旧图像照旧各干各的,一张管的是任务切换,一张管的是环三进出,谁也不越谁的界。fxsave 和 fxrstor 本身就是通用寄存器加内存的指令,是不碰 xmm 的,所以守闸的代码自己编进岛里,守门的不许用 SIMD,这一层没有鸡生蛋的问题。

闸门的深浅,是这道活里最见功力的地方,头一版设计咱们想简单了,实施时被抓出来的是两处漏洞,咱们如实交代。头一处咱们说深度计数。嵌套是常态:syscall 的会话里中断随时嵌进来。最初咱们设计的是全局一个深度计数,最外层包办了存和还。纸面上是对的,细想就露了馅:任务 A 的 syscall 没退完就被换下 CPU,全局的深度还停在一,B 头一次进了门,被判成了嵌套,存都免了,B 的用户图像没人保管。孩子头一回直接从返回尾回环三、退场的任务不再回来,也都把计数留在了半空。正解是深度跟着任务走:计数和图像都长在任务的身上,A 换了出去,A 的深度跟 A 走,B 的深度从零开始,误伤不了谁。闸门自己住在两个不长的 C 函数里,进和出各占了一头,咱们把它们整个端出来,从进门的那头看起:

```cpp
extern "C" void EnterFpuGate() {
    const cinux::arch::IrqGuard kGuard;
    auto* const                 kCurrent = cinux::proc::Scheduler::self().current();
    if (kCurrent == nullptr) {
        return;
    }
    if (kCurrent->fpu_gate_depth++ == 0) {
        cinux::arch::SaveFpu(kCurrent->entry_fpu);
    }
}
```

进门的头一拍是 IrqGuard 把中断关严,改深度的这一下是容不得缝的。kCurrent 是空的,咱们就原路回去,那两行是启动早期的空操作,还没立起当前任务的年月,进来出去连现场的边都不碰。真正承重的是 if 那一行:fpu_gate_depth 是长在任务身上的,深度从零到一的那一拍,才把现场存进任务的 entry_fpu,存的是用户的图像。等内层再进了门,深度只往上加了,entry_fpu 是一个字都不动的,内层的现场另有安排,咱们接着说。

```cpp
extern "C" void ExitFpuGate() {
    const cinux::arch::IrqGuard kGuard;
    auto* const                 kCurrent = cinux::proc::Scheduler::self().current();
    if (kCurrent == nullptr) {
        return;
    }
    cinux::base::safety::Check(kCurrent->fpu_gate_depth != 0, "unbalanced FPU gate exit");
    if (--kCurrent->fpu_gate_depth == 0) {
        cinux::arch::RestoreFpu(kCurrent->entry_fpu);
    }
}
```

出门是进门镜子里的那一头,深度减回了零才还原,还原的还是同一份 entry_fpu。多出来的是那声 Check。咱们拿它当断言的家伙:出的比进的多,深度就减到了零以下,它当场把失衡叫了出来,不会悄悄地放过去。

第二处管的是内层的现场。光保用户的图像还不够:syscall 在内核里跑着的当口,内核自己算到一半的 xmm 也是活现场,这时中断嵌了进来,内层要是只顾门规不存不取的话,中断一路的 SSE 跑完,外层的内核计算就毁了。正解是内层入口各在自己的栈上另存一张被打断的现场,还原的事放在返回之前。咱们看中断入口的真身,如今是这个样子的:

```cpp
template <unsigned int Vector>
__attribute__((interrupt)) void irq_entry([[maybe_unused]] InterruptFrame* frame) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    cinux::arch::FpuImage interrupted_fpu;
    cinux::arch::SaveFpu(interrupted_fpu);
    EnterFpuGate();
    cinux::interrupt::Irq::self().dispatch(
        cinux::interrupt::IrqLine{.value = Vector - kFirstIrqVector});
    cinux::proc::Scheduler::self().maybe_preempt();
    ExitFpuGate();
    cinux::arch::RestoreFpu(interrupted_fpu);
}
```

interrupted_fpu 是函数里的局部变量,落在这一层中断自己的内核栈上,自己的栈上另存一张,说的就是它。存的是被打断那一刻的现场:打断的要是用户态,它跟 entry_fpu 存的是同一副,打断的要是内核算到一半的 xmm,它救的就是那半截活。中间的 EnterFpuGate、ExitFpuGate 照旧走深度:这一层要是嵌在 syscall 的里面,深度就从一加到了二,谁的图像都不碰,这一层的还原,靠的是末尾紧挨着的 RestoreFpu。异常那一头的 ReportFault,头尾夹的是同一个章程,咱们不重贴了。一层只管一层的事,每层多花的是五百一十二字节内核栈,买的是各层现场都完好。

还有进门次序的讲究,咱们看 syscall 真门上的那几行:四格银行存完了,紧跟着的就是闸门。

```asm
    movq %r15, %gs:56
    call EnterFpuGate
    sti
    movq 24(%rsp), %rdi
    movq 32(%rsp), %rsi
    movq 40(%rsp), %rdx
    movq 48(%rsp), %rcx
    call InvokeSyscall
```

call EnterFpuGate 排在 sti 的前头,闸门立好了咱们才放中断进来,顺序倒过来的话,开着口的窗口里进来一个中断,它存走的就还不是立好闸的现场。正事办完了,出门的那一段是这样的:

```asm
    cli
    pushq %rax
    subq $8, %rsp
    call ExitFpuGate
    addq $8, %rsp
    popq %rax
    movq 32(%rsp), %rdi
    movq 40(%rsp), %rsi
    movq 48(%rsp), %rdx
    movq 56(%rsp), %r10
    movq 64(%rsp), %r8
    movq 72(%rsp), %r9
    movq 8(%rsp), %rcx
    movq 16(%rsp), %r11
```

cli 打的头,关严的事在前,动现场的事在后。中间的五拍是 rax 的保存舞步:ExitFpuGate 是个 C 函数,caller-saved 的寄存器它会砸,偏偏此刻 rax 里装的是 InvokeSyscall 交回来的返回值。您数后面那一串重装,rdi 到 r9、rcx 到 r11 的每一位,个个都从帧里搬了回来,唯独不见 rax 的影子。帧上 rax 的那个槽位记的是进门的号码,不是出门的成绩,返回值是只在寄存器里活着的,只好自己 push 了再自己 pop。subq 那一拍是给 call 配的对齐:call 自己压的是一枚返回地址,约定要求被调的函数进门时栈顶除十六余八,咱们 push 完 rax 再让出八字,call 把返回地址也压了回去,就恰好齐了。

fork 那桩连带的脏图像,咱们顺手在同一道闸里清了:孩子不再现场抓快照,改抄父任务的进场图像。fork 只能从 syscall 的门进,进场图像在那一拍必然是父任务真实的用户状态,孩子抄走的,出生就是干净的。机内的用例特意种了与现场不同的 XMM 魔数,咱们逐字核对孩子继承的图像,连孩子从深度零起步也一并验了。

验证咱们两头做。宿主上立了四案:启动期空操作不碰活寄存器,嵌套进出的还原只认最外层,两个任务在闸内交错各自的图像与深度互不串,中途直接退出的任务,后继是不受牵连的。这套用例自己禁了 SIMD,免得探针改写它要查的寄存器,驱动靠的是测试注入的调度替身,跑的是真闸门和真的存取指令,验证的边界也照实说:它不冒充真实硬件的上下文切换。真正的机器上,咱们请出 mapper,这一卷新写进 /bin 的测试程序,让它在文件映射的头一次读取之前站好位:咱们给 xmm0 到 xmm7 装上十六个互不相同的魔数词,把装载、触发读取、取样三步写在了同一个 asm 块里,编译器想插手的缝都没有。读完了对拍,要求立的是:一个词都不能变。再补的是两路探针:一发系统调用来回,回来的 xmm 得是活的。一发 fork 的探针,父子的 xmm 都要对得上,末了父亲还要收得到孩子的退出码。普通镜像和开着锁名册的检查镜像里各连跑了五回,全过了,退出码交了七,shell 的重生照常。这道闸门护的是往后所有用户程序的浮点现场,今天护的是一个 mapper,往后装库跑别人的程序,人家编译器排出来的 SIMD 指令密度,只会比咱们眼下的密。
