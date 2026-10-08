---
title: 02 · 第二条执行流
description: "切换发生在函数调用边界,caller-saved 寄存器已经死了,要存的只有 callee-saved 八个槽,外加一副 512 字节的 SIMD 现场。十一条 static_assert 把汇编偏移锁进编译期。存档 rip 槽里写的是自家的恢复点,入口地址另有去处;jmp 不是 call,新任务的栈不欠任何人一个返回地址。"
chapter: 12
order: 2
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - process
  - assembly
---

# 第二条执行流

护具穿好了,咱们来办正事:给内核添上第二条执行流。把这句话翻译成机器的语言——所谓多一条执行流,就是让 CPU 的全套寄存器,从一套值换成了另一套值。咱们把通用寄存器、栈指针、指令指针一样一样换过去,换到 rip(指令指针寄存器)的那一瞬,下一条执行的指令就是别人的了。所以第二条执行流的全部家当,其实就是一副寄存器照片。照片存下了,人就走开了,照片装回来了,人就回来了。本节咱们写的就是拍照和装回这两下,它们的住处是 `kernel/arch/x86_64/context_switch.S`,是整个工程头一份独立的汇编文件。

动笔之前有一笔数要算清:照片要拍哪些寄存器?拍全十六个通用寄存器吗?不必,而且不能这么想。咱们把切换安排在一个明确的地点:函数调用的边界上,往后您会看到,任务让出 CPU 走的都是 yield 或中断出口这样的调用链深处。而在调用边界上,x86-64 的调用约定早就替咱们分好了工:一半寄存器叫 caller-saved,归调用者保存——跨一次调用这类寄存器的值随时会死,您要想留着,收好的差事归您自己。另一半的名字叫 callee-saved,被调方保存:被调用的函数保证把这些寄存器原样还回来。咱们要拍的,恰好就是 callee-saved 的这一半:rbx、rbp、r12 到 r15,外加的还有 rsp 和 rip,拢共凑成了八个槽。caller-saved 的那一半不用拍,在调用边界上它们本来就是死的,所以谁也没指望它们活过这次调用。

八个槽的家就安在 `context.hpp` 里,咱们把它的真身摆出来:

```cpp
struct alignas(16) CpuContext {
    unsigned long long r15;      ///< Slot 0, offset 0.
    unsigned long long r14;      ///< Slot 1, offset 8.
    unsigned long long r13;      ///< Slot 2, offset 16.
    unsigned long long r12;      ///< Slot 3, offset 24.
    unsigned long long rbp;      ///< Slot 4, offset 32.
    unsigned long long rbx;      ///< Slot 5, offset 40.
    unsigned long long rsp;      ///< Slot 6, offset 48.
    unsigned long long rip;      ///< Slot 7, offset 56.
    unsigned long long fpu[64];  ///< FXSAVE64 image, offset 64.
};
```

八槽之外还拖着一副 512 字节的 fpu 数组,咱们放在后面单独说。您看结构体背后立着的十一条 static_assert:汇编里写的每一个偏移(r15 在 0、rbp 在 32、fpu 在 64),咱们在头文件里逐条断言。这是汇编护栏的写法:两份文件各自认一个数,分家只是早晚的事,与其等某天改了结构体、汇编还按旧偏移读写把任务的照片存串位,不如让编译器每一秒都替咱们对表。偏移这类东西不该由咱们亲手数第二遍。

然后咱们请出正主,ContextSwitch 的全文:

```asm
ContextSwitch:
    movq %r15, 0(%rdi)
    movq %r14, 8(%rdi)
    movq %r13, 16(%rdi)
    movq %r12, 24(%rdi)
    movq %rbp, 32(%rdi)
    movq %rbx, 40(%rdi)
    movq %rsp, 48(%rdi)
    leaq .Lresume(%rip), %rax
    movq %rax, 56(%rdi)
    fxsave64 64(%rdi)

    movq 0(%rsi), %r15
    movq 8(%rsi), %r14
    movq 16(%rsi), %r13
    movq 24(%rsi), %r12
    movq 32(%rsi), %rbp
    movq 40(%rsi), %rbx
    movq 48(%rsi), %rsp
    fxrstor64 64(%rsi)
    jmp *56(%rsi)

.Lresume:
    ret
```

上半场拍照:rdi 指着旧任务的照片,八个槽挨个儿地存。您留意 rip 那一槽:存进去的是 `.Lresume` 的地址,自家代码里的一个标号。这是整份汇编里最要紧的心思:旧任务的照片里,指令指针不指向它被换走时正在跑的那行代码,而指向切换函数自己的恢复点。往后哪天咱们把它换回来,装完八槽的一跳落在 `.Lresume`,那儿躺着的只有一条 ret。于是对 C++ 的世界来说,ContextSwitch 就是一次普通的函数调用:调进去,过了很久很久,正常返回了。被切走的代码根本不知道自己离开过多久。下半场装照片:rsi 指着新任务的照片,八槽装了回来,fxrstor64 把浮点现场也装了回来,末了 `jmp *56(%rsi)` 跳进新任务照片里的 rip。这里为什么是 jmp 不是 call?call 会在当前栈上压一个返回地址,可这个栈马上就要被换成新任务的了——压进去的返回地址,就落在了别人的栈顶,那是纯粹的污染。jmp 干净利落:走过去就完了,谁的返回都不欠。

该说那 512 字节了。会疼那一卷解锁 SSE 的时候,咱们就说过向量化的门开了,编译器会给普通的 C++ 代码生成 XMM 指令,咱们随手写个循环把数组倒腾两下,中间结果就住在 xmm0 到 xmm15 的寄存器里。两个任务各自的中间结果不能互相看见,所以切换时 SIMD 现场也得整副带走:fxsave64 把 x87 和 SSE 的整套状态(寄存器加控制字)一口气存进 512 字节,原样装回的差事归 fxrstor64。这也解释了结构体上那个 alignas(16):fxsave64 和 fxrstor64 对操作数有 16 字节对齐的硬要求,结构体自身站齐在 16 的倍数上,槽 64 的 fpu 才永远落在合法的位置上。这个 16 咱们现在记下,它是本卷稍后一笔旧债的债主。还有一层关系咱们交代在前:SIMD 现场是任务与任务之间的差别,切换的时候就得保住。而中断链路自己会不会弄脏被打断者的 XMM,是另一回事——咱们靠不开向量化的岛办到:从中断桩到调度器的一整条链,连一个 XMM 的指令都不产生,现场当然不脏,岛上的住户名单,本卷又收进了 proc 全家。

照片的架子立好了,可照片从哪来、新任务的第一跳跳到哪,在咱们这儿都还没着落。下一节咱们造任务:栈、身份、还有那座承上启下的蹦床。
