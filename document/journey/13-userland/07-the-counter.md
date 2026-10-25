---
title: "07 · 柜台与三条约定"
description: "ring3 碰不到真东西:写字、等键、退场,都得过内核的手。SYSCALL 这条快通道寄存器直达,不查表、不压帧,入口汇编 swapgs 换 GS、GS 换栈,十二槽帧立在任务的内核栈上,切换业务才做得成。编号、寄存器、负 errno,三条约定一次定死贴 Linux。分发表 constexpr 落座,空洞返 -ENOSYS。write 两道检查,立下内核不盲信客人指针的底线。"
chapter: 13
order: 7
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - syscall
  - userland
---

# 柜台与三条约定

ring3 的手里一件真东西都没有,写字、等键、退场靠的全是内核地界里的东西,客人的每一次伸手,特权级的那一关都会拦下。这一章咱们给内核立一座柜台,客人的话从那头递过来,咱们在这头接活。走的是 SYSCALL 与 SYSRET 这对快通道:寄存器直达、不查 IDT、不压帧,连栈都是不换的——门轴开在 MSR 里,栈的事得自己管,所以首跳那一章才在 GS 落脚点里预铺了 kernel_rsp 的路,座位那一章的 STAR 与 SCE 点亮的钥匙,也全是替它备下的。

咱们赶在业务动手之前,把三条约定一次定得死死的,而且不是自己拍脑袋——编号、寄存器、返回值,全部照着 Linux x86_64 的原样。为什么让别人定?这套约定不是咱们一家的私事:将来把 musl 装进来跑 busybox 的那一卷,人家编译好的二进制就按这套编号敲门、按这套座位递话、按这套格式读回执,约定错了的话,到那天的每一行都得返工。头一条定的是编号:read 的号是 0,write 的号是 1,exit 的号是 60,yield 拿的,正是 Linux 留给 sched_yield 的 24 号。下一条定的是寄存器:rax 里装的是编号,参数的座位是 rdi、rsi、rdx、r10、r8、r9 六个,rcx 与 r11 不在名单里——它们被 SYSCALL 的硬件征用了,第四参数才挪去了 r10。末一条定的是返回值:成功给的是正数,写的字节数、零都算。出了错的话,给的就是负的 errno:ENOSYS 38、EBADF 9、EFAULT 14,这些码的出处全是一张词表。当年头一遍返的是光秃秃的 -1,要到装库的那一天才发现得返工,这一回咱们一次定对。

errno 这个词咱们头一回正经用,值得咱们停下来讲明白。它是 error number 的缩写,C 世界拿它给错误编的号,Linux 给每一种错误起了名字、配了整数。咱们约定返负 errno:出错时把这个号取了负再还回去,您拿到的是负数,查一查手里的号,错的是哪一种也就对上了。这一卷咱们会用上三个。ENOSYS 拿的是 38 号,字面说的就是没有这项系统调用,分发表里的空座返的就是它。EBADF 拿的是 9 号,管的是坏的文件描述符。描述符是客人递进来的那个编号,写往不是 1 的号、读往不是 0 的号,返的都是它。EFAULT 拿的是 14 号,管的是坏的地址,客人递来的指针伸进了不该去的地界,比如内核的高半区。号是 Linux 定好的,咱们原样借来,北极星认的就是这套。

咱们把入口写成了一段汇编、把前半截请了出来,您跟着一行行看:

```asm
SyscallEntry:
    swapgs
    movq %rsp, %gs:8
    movq %gs:0, %rsp
    pushq %rbp
    # —— 中间十拍,余下的寄存器一件一件压齐 ——
    pushq %gs:8
    sti
    movq 24(%rsp), %rdi
    movq 32(%rsp), %rsi
    movq 40(%rsp), %rdx
    movq 48(%rsp), %rcx
    call InvokeSyscall
```

开头的 swapgs 把 GS 的两个 base 对调。咱们眼下两个 base 指的是同一块,换了等于没换,可这一拍省不得:将来客人有了自己的 TLS,它就是两个世界的分界线。第二、三条走的正是首跳那一章铺好的路:用户栈顶暂存进 gs:8,内核栈是从 gs:0 里取的——syscall 落栈走的是 GS,ring3 来中断换栈走的才是 TSS.rsp0,两扇门是同源的。接着压了十二拍栈:九十六字节的帧,把客人的全套寄存器加栈顶原样存了档,偏移的断言照家法逐条对死。

十二个座位哪个坐的是谁,咱们列成一张表。压栈的头一拍是 rbp,坐上最高的第 11 槽,末一拍把暂存的用户栈顶压了进去,落在了第 0 槽,中间的十拍从 rbx 一路往下坐到 rcx:

```text
槽 0    用户栈顶        0(%rsp)
槽 1    rcx             8(%rsp)
槽 2    r11             16(%rsp)
槽 3    rax             24(%rsp)
槽 4    rdi             32(%rsp)
槽 5    rsi             40(%rsp)
槽 6    rdx             48(%rsp)
槽 7    r10             56(%rsp)
槽 8    r8              64(%rsp)
槽 9    r9              72(%rsp)
槽 10   rbx             80(%rsp)
槽 11   rbp             88(%rsp)
```

帧立稳了才轮到 sti 那一拍。SYSCALL 进门的那一刻,SFMASK 里写好的 0x200,进门就会把 IF 清掉——这是设计:换栈建帧的中途,一个中断都是不许来的,帧立好了之后内核才敢开。LSTAR 里存的就是这段入口的地址,SFMASK 里存的就是要清的标志位。末了的四拍搬参数:编号住的是 rax 的槽,头一拍搬进 rdi(C 的第一个参数走的正是它),后三件座对座地搬运,请 C++ 那一侧的 InvokeSyscall 上工。咱们让用户栈顶坐在帧的第 0 槽,不走回读 gs:8 的路子,因为 InvokeSyscall 的里头可能切任务,帧是跟着任务走的,gs:8 却会被下一个进门的 ring3 任务盖掉。

咱们在 C++ 那一侧摆的是张六十一格的表,编号从 0 排到了 60,一格坐的是一个函数。表是 constexpr 函数挨个入座的,在编译期就定得死死的。read 的座今天还空着,留给后头要读键的常客。编号超界的、编了号没人坐的,返的都是 -ENOSYS——柜台不装懂,没这项业务的话就把话说明白。write 实装了,进门的两道检查:描述符不是 1,返的是 -EBADF。缓冲区指针越过了用户半边界,返的是 -EFAULT——高半区是内核的地界,客人递来的指针,内核是不盲信的,这道底线从柜台开张的头一天就立下了。yield 干的事是把 CPU 还给调度器,exit 走的是收尸路,一去就不回头了。

柜台立起来了,还得让任务进得了门、也出得了门——切换器也就跟着升了一级。Task 身上添了一个字段 user_root:零是内核任务的记号,非零就是它自己的世界根。咱们让切换器在请任务上 CPU 之前,对齐的是三处:TSS 的 rsp0,落脚点的 kernel_rsp、还有 CR3。两扇换栈的门背后,从此通着任务自己的内核栈顶,CR3 已是对的就不再重写,回了家才换回内核根。然后 hello 改了行:首跳那一章它是验收员,今天柜台开张了,它写的是两行字、一次退场。启动的活搬进了任务里,Main 放好了座位,run_until_done 的一声令下,客人下了楼。

客人那边也得有自己能说的话,咱们把递话的这一侧写进了 user/api/syscall.hpp:不带一行运行时,只有一条共用的内联 syscall,包出来的是 Write、Read、Yield、Exit 四个名字。咱们从共用的内联 syscall 看起:

```cpp
inline long long Syscall3(unsigned long long number, unsigned long long arg1,
                          unsigned long long arg2, unsigned long long arg3) {
    // NOLINTNEXTLINE(misc-const-correctness)
    long long ret = 0;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3)
                     : "rcx", "r11", "memory");
    return ret;
}
```

这座桥就是一条内联汇编搭起来的。咱们顺着约束读:编号进的是 a,三个参数各坐各的座位,D 管的是 rdi、S 管的是 rsi、d 管的是 rdx,回执还是从 a 里出的。这一卷的四笔业务最多吃三个参数,所以咱们只拼了头三个座位,第四座位 r10 往后的,等真有用得上的业务再扩。四个名字都是它的薄包装,咱们贴出来看:

```cpp
inline long long Write(unsigned long long descriptor, const void* buffer,
                       unsigned long long count) {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kWrite), descriptor,
                    reinterpret_cast<unsigned long long>(buffer), count);
}

inline long long Read(unsigned long long descriptor, void* buffer, unsigned long long count) {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kRead), descriptor,
                    reinterpret_cast<unsigned long long>(buffer), count);
}

inline long long Yield() {
    return Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kYield), 0, 0, 0);
}

[[noreturn]] inline void Exit(unsigned long long code) {
    static_cast<void>(
        Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kExit), code, 0, 0));
    __builtin_unreachable();
}
```

包装里没有一笔硬写的数字,编号全部来自内核头文件里的 SyscallNr 枚举,kWrite 对应的就是 1,kExit 对应的就是 60,客人和内核两个世界读的是同一张表,谁要是改了一个号,两边就一起跟着变了。Exit 头上的两个记号,您也顺带看一眼:[[noreturn]] 与 __builtin_unreachable 的组合,向编译器声明了没有回程,后面的路径就能放心地裁掉了。

咱们回到 Syscall3,整段最承重的是清单末行的三位:`"rcx", "r11", "memory"`。这是咱们向编译器许下的话,也正是 Linux 向用户态许的话:过门会换的只有 rax,会丢的只有 rcx 与 r11,其余的寄存器一律原样奉还。头两位报的是硬件的征用,进门那一刻 rcx 被塞了返回地址、r11 被塞了标志,所以它们进不了输入输出的名单。末位的 memory 报的是门里写过内存,编译器也就不许把一次加载挪到进门的前头。这句话咱们记下了,下一节的对质,指着的就是它。

咱们按下运行。串口上落下了一行:

```text
[user] hello from ring 3
```

剩下的收尾交给了 exit:任务排干,Main 收了尸、回了魂,面板与串口自始至终没有一句抱怨的话。写字的、退场的,头两笔业务都走通了,yield 的座在表里留着,等的是将来的常客。照咱们看,一切也都是对的。
