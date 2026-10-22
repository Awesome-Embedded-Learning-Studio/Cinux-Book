---
title: "02 · 长全的帧,关门的银行"
description: "十二拍九十六字节长到十六拍一百二十八字节,r12 到 r15 从 GS 银行搬回帧内,per-CPU 块从八格缩回四格,fork 改从帧里取孩子的四个寄存器。入口汇编一句 movq 把整帧递给 InvokeSyscall,Handler 的脸统一成吃帧,五参六参的 syscall 由 handler 自己伸手到帧里取。Handler 脸是只读的,exec 与 sigreturn 两处写帧在体内取回写权,分派对其余调用承诺只读。"
chapter: 21
order: 2
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - syscall
  - signals
---

# 长全的帧,关门的银行

柜台那一卷立帧的时候,章程定的是十二拍、九十六字节,把客人的寄存器存档上栈。生与收那一卷的进度里,咱们发现帧里没有 r12 到 r15,而 fork 偏偏离不开它们,当时的解法是帧一处不动,在 per-CPU 块里开了个小银行,入口汇编压完帧、还没开中断的时候顺手存四格。今天咱们动这个约定,动以前咱们把道理讲明白。

信号要的远不止借一眼寄存器,它要的是接手整个执行现场。咱们喊停一个任务,得让它停下正在跑的用户代码,回头去办别的事,办完了再从断点接上。这个接手落在哪?落在回用户态的路上,而回用户态走的现场,就是咱们的帧。咱们要接手,帧里就得有全部的通用寄存器,少了哪一格都接不成手。您会问,咱们不是还有中断帧吗?咱们的中断帧是编译器按门脸生成的,五个字段的身子,住着的只有 rip、cs、rflags、rsp、ss,通用寄存器是没有位置的,想改写都找不到落笔的地方。所以正路只剩一条:把 syscall 帧缺的四个格子补上。咱们把入口汇编新的头四拍抄来:

```asm
    movq %gs:0, %rsp
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %rbp
```

新来的四拍打头,r15 到 r12 倒着压了进去,占的是帧里最高的四个新槽:r12 住的是九十六,r13 住的是一百零四,r14 住的是一百一十二,r15 住的是一百二十。往下的十二拍一拍没动,末一拍照旧坐的是用户栈顶。十二拍长到了十六拍,九十六字节长到了一百二十八字节,布局锁跟着改了口,新立的断言咱们抄一条给您,它的原话是 r12 closes the sixteen-word frame,翻过来就是 r12 关上了十六词的帧。帧从此是任务在内核期间的整套用户寄存器档,往后谁要读任务的现场,读它就是了。

帧全了,银行就该关门了。per-CPU 块从八格缩回了四格,六十四字节变成了三十二字节,gs:32 到 gs:56 的四格整个删掉,留下的还是老四样:内核栈顶、用户栈顶、一格过渡、帧的地址。fork 那边的改法顺理成章:BuildForkChild 还是收帧的地址,取孩子的 r12 到 r15 却不再问银行,直接从父进程的帧里读。当年每次 syscall 白存四个 mov 的日子,到今天结清了。您回头翻生与收那一卷,还会读到一句更远的话,说两个 GS base 同值的简化,等用户态真用自己的 GS 基址那天再说。帧的事与它无关,那句话咱们接着挂着。

帧变宽只是地基,真正的改型在 Handler 的脸。系统调用这一侧的办事人,代码里管它叫的是 Handler,咱们照着代码叫。句柄这个词咱们留给信号那边,专指装进用户程序里的办事人,两边是不混的。从前的分派里,入口汇编干的活是从帧里搬四个参数进寄存器,InvokeSyscall 收的是号加三个词。现在这些搬运全省了,入口的一句 movq %rsp, %rdi 把整张帧递了进去,咱们把 C 那一侧的正文抄来:

```cpp
extern "C" long long InvokeSyscall(cinux::syscall::SyscallFrame& frame) {
    const unsigned long long kNumber = frame.rax;
    if (kNumber >= cinux::syscall::kSyscallCount || kHandlers[kNumber] == nullptr) {
        return -cinux::syscall::kEnosys;
    }
    const long long kResult = kHandlers[kNumber](frame);
    frame.rax               = static_cast<unsigned long long>(kResult);
    if (cinux::proc::DeliverPendingSignals(frame)) {
        return static_cast<long long>(frame.rax);
    }
    return kResult;
}
```

您看这段里埋着两处伏笔。头一处讲的是 Handler 的脸,它统一成了吃帧:号住在帧的 rax 槽里,参数散在 rdi 往后的槽里,所以要用第几个,Handler 自己就伸手到槽里取了。懒到家那一卷实弹炸过的第四参没落 r10,那一枪的病根从此拔了:第五参第六参的 syscall,后面的卷里 clone 要五个、futex 要六个,门已经在帧里开好了,谁也不用再改 InvokeSyscall 的脸。整个系统调用面三十八处签名跟着换了装。Handler 的脸还是只读的,吃的是常量帧,真要写帧的只有两处,exec 的换世界和 sigreturn 的回现场,它们在自己体内把写权取了回来,分派对其余的 Handler 承诺的是只读。第二处的伏笔,是分派把返回值写回帧的 rax 槽之后,还多问了一句投递的话,那就是下一节的主角,咱们这里把地基验完。

入口汇编的收尾也跟着长了一笔。InvokeSyscall 回了来,咱们让汇编把 rax 压栈护住,送它过了一遍浮点闸门,又弹了回来,然后照旧逐拍地弹帧回家。新添的是护住以前的那一笔:咱们把返回值直接写进帧的 rax 槽。从前的帧,回程之后 rax 槽里躺的还是进门时的号,反正马上就弹出去了,读它的人是没有的,所以咱们也无所谓。现在帧是任务的寄存器档了,无论什么时刻去读,读到的都该是真话,所以答案一出来咱们就归了档。ForkChildEntry 的镜像返回段同样补了四拍弹射,孩子在帧里捧着的,从今天起也是完整的十六词。

地基验完了,咱们把这一篇的家底点清:十六拍的帧,三十二字节的 per-CPU 块,吃帧的分派,随时是真话的寄存器档。票子都备好了,下一节咱们开始发行:号表、集合,还有两个谁也不许碰的例外。

