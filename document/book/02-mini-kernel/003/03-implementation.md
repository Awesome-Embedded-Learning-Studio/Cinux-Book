---
title: 03 · 代码路线:GDT、IDT、ISR stub、伪错误码、C handler、门类型
---

# 代码路线:GDT、IDT、ISR stub、伪错误码、C handler、门类型

## 1. 内核自己的 GDT:换掉 bootloader 的临时表

[gdt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/arch/x86_64/gdt.cpp) 建一张三项的扁平 GDT——空段、64 位代码段、64 位数据段,和 003 进长模式那张是同一种"扁平模型",只是现在由内核自己拥有:

```cpp
s_gdt[GDT_NULL_INDEX]  = make_gdt_entry(0, 0,       0,    0);     // 空
s_gdt[GDT_CODE64_INDEX]= make_gdt_entry(0, 0xFFFFF, 0x9A, 0x0A);   // 代码:access 0x9A, flags 0x0A(L=1)
s_gdt[GDT_DATA64_INDEX]= make_gdt_entry(0, 0xFFFFF, 0x92, 0x0C);   // 数据:access 0x92, flags 0x0C
```

`0x9A` 是"present + ring0 + 代码段 + 可读",`0x0A` 的高位 flags 是 `G=1, D=0, L=1`(L 位=1 才是 64 位代码段);数据段把"可执行"去掉得 `0x92`。base 全 0、limit 全 1——扁平,段透明。选择子 `SEGMENT_CODE64 = 1*8 = 0x08`、`SEGMENT_DATA64 = 2*8 = 0x10`。

`gdt_init` 把表填好后,`lgdt` 加载,然后用 002 那套"压目标 CS + 压返回地址 + `lretq`"的远返回把 `CS` 切到 `0x08`,再 `mov` 重载 `DS/ES/FS/GS/SS`。这套刷新流程 002 讲过原因(长模式不能 `mov cs`),这里只是内核在自己的地址空间里重做一遍。

## 2. IDT:给异常一个"入口地址表"

GDT 就位,IDT 才有可靠选择子可用。[idt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/arch/x86_64/idt.cpp) 先把 256 项清空(`Present=0` 表示未用),再只填我们关心的两个向量:

```cpp
set_idt_entry(IDT_VEC_BP, isr_bp_stub, SEGMENT_CODE64, 0x8F, 0);  // #BP 陷阱门
set_idt_entry(IDT_VEC_PF, isr_pf_stub, SEGMENT_CODE64, 0x8E, 0);  // #PF 中断门
```

`set_idt_entry` 把处理程序地址(这里是汇编 stub `isr_bp_stub` 的地址)拆成低/中/高三段填进 `IdtEntry`,记下选择子 `0x08`、IST=0、`type_attr`。填完 `lidt` 加载,IDT 就生效了。

注意填进去的是**汇编 stub 的地址**,不是 C 函数 `handle_bp` 的地址。CPU 进异常时,栈上的状态是固定的(错误码、rip、cs、rflags、rsp、ss),和 C 的调用约定(参数走 `rdi`)对不上,不能让 CPU 直接跳进 C 函数——中间必须有一段汇编 stub 做适配。这就是下一节。

## 3. ISR stub:保存现场、传 InterruptFrame*、iretq 回去

[interrupts.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/arch/x86_64/interrupts.S) 用两个宏生成 stub。`ISR_NOERRCODE`(给 #BP 这种没有硬件错误码的异常用)的骨架是:

```asm
.macro ISR_NOERRCODE name vector handler
\name:
    pushq $0              # ① 补伪错误码 0,和有错误码的异常对齐栈帧
    pushq %rax            # ② 保存全部通用寄存器(rax 先压、…、r15 最后压)
    ...
    pushq %r15
    movq %rsp, %rdi       # ③ 栈顶现在指向 r15 → 作为 InterruptFrame* 传第一参
    call  \handler        # ④ 调 C 处理函数
    popq  %r15            # ⑤ 按相反顺序恢复
    ...
    popq  %rax
    addq  $8, %rsp        # ⑥ 跳过伪错误码
    iretq                 # ⑦ 中断返回
.endm
```

这里最要紧的是**压栈顺序和 C 结构的对应**。stub 先 `push %rax`(rax 落在最高地址),一路压到 `push %r15`(r15 落在最低地址,也就是此刻 `rsp` 指向的地方)。而 [idt.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/arch/x86_64/idt.hpp) 结构体从低地址往高地址定义的字段是 `r15, r14, ..., rax, error_code, rip, ...`——正好 `rsp` 指向 `r15`。所以把 `rsp` 当 `InterruptFrame*` 传给 C,C 里 `frame->r15` 读到的就是被中断时的 r15,一一对应。这个顺序一旦压反,C 读到的寄存器全是错位的(RAX 显示成 RBX 的值),极难察觉。

`ISR_ERRCODE` 宏(给 #PF)几乎一样,只省掉第 ① 步——因为 #PF 的错误码是 CPU 进入时自动压的,不用 stub 补。两个宏最后都是 `addq $8,%rsp` 跳过(伪或真)错误码,再 `iretq`。

## 4. 伪错误码:#BP 为什么要在栈上补一个 0

为什么要给 #BP 这个"没有错误码"的异常硬塞一个 0?为了让**两种异常的栈帧布局统一**。

有错误码的异常(像 #PF),CPU 进入时栈上是 `错误码 / rip / cs / rflags / rsp / ss`;没错误码的(像 #BP),栈上直接是 `rip / cs / rflags / rsp / ss`,少了一项。如果 stub 不补,那么 `handle_bp` 拿到的 `InterruptFrame*` 和 `handle_pf` 拿到的,字段就对不齐——同样是 `frame->error_code`,一个读到的是真错误码、另一个读到的其实是 rip。补一个伪 0 之后,两种异常的 `InterruptFrame` 布局完全一致,C 处理函数不用区分对待。stub 末尾 `addq $8` 跳过它,刚好抵消。

## 5. C handler:#BP 继续,#PF 读 CR2 + 解错误码

[exception_handlers.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/arch/x86_64/exception_handlers.cpp) 的两个函数都拿到 `InterruptFrame*`,先调 `dump_interrupt_frame` 把全部寄存器打到串口,然后各自处理。

`handle_bp` 很简单:dump 完就返回。因为 `int $3` 是陷阱,压栈的 `rip` 已经指向下一条指令,`iretq` 返回后内核从 `int $3` 的下一条继续——这就是"断点不死机"的全部。

`handle_pf` 多两步,先从 `CR2` 读出导致缺页的线性地址(这是 CPU 在 #PF 时自动写进 CR2 的),再解析 `frame->error_code` 的各个位:

```cpp
uint64_t fault_addr;
__asm__ volatile("movq %%cr2, %0" : "=r"(fault_addr));
const char* present = (err & 0x01) ? "protection violation" : "page not present";
const char* access  = (err & 0x02) ? "write" : "read";
const char* mode    = (err & 0x04) ? "user" : "kernel";
...
```

bit0 是"不存在 vs 权限冲突"、bit1 是"读 vs 写"、bit2 是"内核 vs 用户"、bit3 是保留位冲突、bit4 是取指缺页。这一章 `handle_pf` 只**打印不修复**——它不去做缺页换页(那是以后 VMM 的事),只是把"哪儿、为什么缺页"说清楚。对一个调试中的内核,这已经足够救命了。

## 6. 陷阱门 vs 中断门:0x8F 与 0x8E 的区别

回到 IDT 那个 `type_attr` 字节。`0x8F` 和 `0x8E` 只差最后一位:门类型 `0xF`(陷阱门)vs `0xE`(中断门)。`0x8` 的高位是 `Present=1, DPL=0`。

这一位之差,决定 CPU 进入处理程序时**要不要清 IF(中断允许标志)**。中断门(`0xE`)进入时 CPU 自动清 IF,整个处理过程不再响应其它中断,出来 `iretq` 时再恢复;陷阱门(`0xF`)不清 IF,处理期间仍可被(更高优先级的)中断打断。

Cinux 的选择是:#BP 用陷阱门 `0x8F`(断点处理时允许嵌套),#PF 用中断门 `0x8E`(页错误处理时不希望被打断,把 IF 清掉更稳)。这种"故障/错误类用中断门、调试/软中断类用陷阱门"的搭配是常见做法。

顺带纠一个源码注释的笔误:`0x8F` 高位的 `8` 表示 `Present=1, DPL=0`,所以这个 #BP 门其实只有 ring0 能触发。但 [idt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/arch/x86_64/idt.cpp) 那行注释写着"DPL=3 允许用户态 INT3"——那是不对的,要真让用户态 `int $3`,得把 DPL 设成 3,也就是用 `0xEF`。当前 mini kernel 只有 ring0,这个差别无所谓,但别被那条注释带偏。
