---
title: 010b · 大内核的 IDT 与异常
---

# 010b · 大内核的 IDT 与异常:给内核装上"异常安全网"

> 这是 010 的下半篇。上一篇我们给 big kernel 铺好了段地基,`TR` 也挂上了 TSS。可你要是这会儿故意往代码里塞一句 `int $3`,内核会直接三重故障重启——因为还没有 IDT,没有任何东西接得住异常。这篇就干这一件事:建 IDT、写 ISR、让 `int $3` 被接住、把寄存器 dump 出来,然后活着继续跑。

## 这一章我们要点亮什么

我们要让 big kernel 第一次拥有"扛事"的能力。落到一个看得见的效果上:在 main 里执行一句 `int $3` 触发断点异常,串口打印出完整的寄存器 dump,然后——这是重点——内核没死,它从异常里返回,接着往下跑,吐出一句 `Breakpoint returned, continuing`。

听起来不起眼,但对一个裸奔的内核来说,这是从"一碰就碎"到"能自我诊断"的分水岭。没有这层兜底,后面随便一个 #PF、#GP 都会换来一次莫名其妙的重启,连死因都看不着。

## 为什么现在需要它

009 时候的 big kernel,本质上是个只会 `kprintf` 的裸奔程序。它脚下的临时 GDT 够它蹭两步,可 CPU 一旦抛出任何异常——除零、缺页、非法指令——没有 IDT 就意味着没有 handler,CPU 找不到落点,连续触发下去就是三重故障,QEMU 直接重启。调试这种状态是噩梦,因为你根本不知道自己错在哪。

我们要做的,是在内核和"彻底崩溃"之间塞进一张安全网。这张网就是 IDT,中断描述符表:它告诉 CPU,"当异常 X 发生时,去调用这个函数"。而 IDT 里每个 gate 最终都要跳进一段代码段去执行——这就用上了上一篇铺好的 GDT 选择子。所以两篇的顺序是锁死的,GDT 先就绪,IDT 才接得上。

## 设计图

先看 IDT gate 长什么样。它和 GDT 的段描述符不一样,是 16 字节一个,干的事就一件:把"异常发生时该跳去哪"编码进去。

```text
字节:    0        2        4     5          6        8         12        16
      ┌─────────┬────────┬─────┬──────────┬────────┬─────────┬─────────┐
      │ offset  │selector│ IST │ type_attr│ offset │ offset  │ reserved│
      │  low    │  (CS)  │     │          │  mid   │  high   │         │
      │ (16b)   │ (16b)  │(8b) │   (8b)   │ (16b)  │  (32b)  │  (32b)  │
      └─────────┴────────┴─────┴──────────┴────────┴─────────┴─────────┘
```

handler 的地址被拆成三段(低 16、中 16、高 32)塞进 gate,因为一个 64 位地址没法直接放进连续字段;selector 是这个 handler 要跑在哪个代码段,就是我们上一篇的 `GDT_KERNEL_CODE`;IST 决定它用哪个中断栈;type_attr 编码"这是个什么 gate"。

异常一来,CPU 干的事大致是这样一条链:先把 SS/RSP/RFLAGS/CS/RIP(再外加可能的 error code)压进栈,然后按 `IDT[向量号]` 找到 gate,把 gate.selector 装进 CS,跳去 handler。handler 那头是我们写的汇编 stub,它保存通用寄存器,把栈指针当参数传给 C 函数,C 函数读完现场再返回,stub 恢复寄存器、跳过 error code、`iretq`,CPU 就把刚才压的那些东西弹回去,回到被打断的地方接着跑。整条链我们下面逐段拆。

## 代码路线

### 用 scoped enum 把异常和 gate 类型说人话

和 GDT 一个思路,我们不写裸数字。异常向量、gate 类型、特权级,统统做成强类型枚举:

```cpp
enum class ExceptionVector : uint8_t {
    DE = 0,  DB = 1,  NMI = 2, BP = 3,  OF = 4,
    BR = 5,  UD = 6,  NM = 7,  DF = 8,
    TS = 10, NP = 11, SS = 12, GP = 13, PF = 14,
};

enum class IDTGateType : uint8_t {
    Interrupt = 0x0E,   // 中断门:进入时清 IF
    Trap      = 0x0F,   // 陷阱门:进入时保留 IF
};

enum class IDTPrivilege : uint8_t {
    Kernel = 0x00,
    User   = 0x60,      // DPL = 3
};

constexpr uint8_t make_idt_attr(IDTPrivilege priv, IDTGateType gate) {
    return 0x80 | static_cast<uint8_t>(priv) | static_cast<uint8_t>(gate);
}
```

`make_idt_attr` 把 present 位、特权、gate 类型拼成一个字节。内核态中断门是 `0x80 | 0x00 | 0x0E = 0x8E`,而 #BP 用的是用户态陷阱门 `0x80 | 0x60 | 0x0F = 0xEF`——这两个数 `0x8E` 和 `0xEF` 后面会反复出现,记住它们。

### gate 策略:为什么 #BP 用陷阱门,其余用中断门

这里有个设计决策值得展开讲。你看异常表会发现,大部分异常我们挂的是中断门(Interrupt, 0xE),唯独 #BP 和 #DB 挂了陷阱门(Trap, 0xF),这不是随便选的。

差别在一个标志位上,IF,也就是中断使能标志。CPU 进入中断门时会自动把 IF 清掉,意思是 handler 执行期间不再响应新的可屏蔽中断,得等它跑完 `iretq` 才恢复;而陷阱门不清 IF,中断状态原样保留。

对大部分致命异常(#PF、#GP、#DF 这类)来说,我们进 handler 就是去打印现场然后死给你看的,期间当然不希望再被别的东西打断,所以用中断门顺手清掉 IF 是对的。但 #BP 和 #DB 不一样,它们是非致命的——一个断点打完,内核要活着继续跑。要是它俩也清 IF,万一将来我们在中断已经使能的状态下命中断点,IF 就被悄悄改掉了,行为会变得很难解释。所以给它们用陷阱门,保证打完断点回来,中断状态和进去之前一模一样。这种"致命用中断门、诊断用陷阱门"的区分,是这版 IDT 里最值得记住的一笔。

### 数据驱动的路由表

14 个异常怎么挂进 IDT?最笨的写法是堆 14 段几乎一样的 `set_handler`。Cinux 没这么干,它用一张表代替:

```cpp
const Route routes[] = {
    {ExceptionVector::DE,  isr_de_stub,  IDTPrivilege::Kernel, IDTGateType::Interrupt},
    {ExceptionVector::DB,  isr_db_stub,  IDTPrivilege::Kernel, IDTGateType::Trap},
    {ExceptionVector::BP,  isr_bp_stub,  IDTPrivilege::User,   IDTGateType::Trap},
    /* ...中间省略... */
    {ExceptionVector::PF,  isr_pf_stub,  IDTPrivilege::Kernel, IDTGateType::Interrupt},
};
```

每个异常就一行:向量号、对应的汇编 stub、特权、gate 类型,然后一个循环把它们写进 IDT。好处很实在——以后要再加一个异常,只动这张表,不用碰分发逻辑;而且谁是什么 gate、什么特权,一眼看全,正好和上面的 gate 策略对得上。这里有个细节:#BP 那行是 `User` 特权,DPL=3,这是故意的,为的是用户态也能 `int $3` 打断点。

### ISR stub:汇编里的两个宏

stub 是异常进来后第一个落脚的汇编代码,它负责保存现场,再跳去 C handler。这里有个细节必须讲清楚:有些异常 CPU 会自动压一个 error code(比如 #PF、#GP、#DF),有些不会(比如 #DE、#BP)。这会让"保存寄存器之后的栈布局"不统一,而布局不统一,后面读寄存器就会全错位。

Cinux 的办法是写两个宏,把这个不统一抹平:

```asm
.macro ISR_NOERRCODE name handler
\name:
    pushq $0              # 没有 error code 的,塞个假的 0,凑齐布局
    pushq %rax            # 往下保存所有通用寄存器 rax..r15
    /* ...pushq %rbx..%r15... */
    movq %rsp, %rdi       # 栈顶此刻正指向保存的 frame,把它当第一个参数传出去
    call \handler         # 调对应的 C handler
    popq %r15             # 恢复寄存器
    /* ...popq..%rax... */
    addq $8, %rsp         # 跳过那个 error code(真的或假的都占 8 字节)
    iretq                 # 中断返回
.endm

.macro ISR_ERRCODE name handler
\name:
    pushq %rax            # CPU 已经压了真的 error code,这里不用再塞假的
    /* ...同上... */
    iretq
.endm
```

关键就是那个 `pushq $0`:对没有硬件 error code 的异常,我们主动塞个 0,让所有 handler 看到的栈布局完全一致。`movq %rsp, %rdi` 传给 C 的那个指针,就永远指向同一个 InterruptFrame 结构。要是不塞这个 0,handler 解析寄存器时就会整体错位,那种 bug 调起来真能把人逼疯——你看到 RAX 的值其实串到了 RBX 上,还以为是硬件抽风。

### C handler:致命的躺平,非致命的继续

到了 C 这层就清爽了。先有个 `dump_registers`,把 frame 里的 RAX、RBX……一路到 RIP、RFLAGS 全格式化打到串口;然后分两类处置:

```cpp
void handle_bp(InterruptFrame* frame) {
    dump_registers(frame, "#BP", 3);
    kprintf("[EXCEPTION] Continuing...\n");
    // 不 halt,直接返回 → stub 恢复寄存器 → iretq → 内核继续跑
}

[[noreturn]] void fatal_halt() {
    while (1) { __asm__ volatile("cli; hlt"); }
}

void handle_pf(InterruptFrame* frame) {
    uint64_t fault_addr;
    __asm__ volatile("movq %%cr2, %0" : "=r"(fault_addr));
    dump_registers(frame, "#PF", 14);
    kprintf("[FATAL] Faulting address (CR2) = %p -- halting.\n", (void*)fault_addr);
    fatal_halt();
}
```

#BP、#DB 这类打完就 `return`,执行流自然回到 stub、回到被打断的地方;其余的一律 `fatal_halt`——`cli` 关中断、`hlt` 停机,死在原地,至少不会带着错误状态继续乱跑造成二次事故。

#PF 这里有个东西值得记一笔:缺页地址不在 frame 里,CPU 把它单独放在 CR2 寄存器,所以 handler 得自己 `movq %cr2, ...` 读出来。error code 那几位也有讲究,第 0 位告诉你"是页不存在,还是保护违规",第 1 位是读还是写,第 2 位是用户态还是内核态触发的。这几位凑一起,就是一封"这次 page fault 到底想干嘛"的说明书,后面真做分页时全靠它定位。

### 串起来:main 里的顺序,和那句要命的注释

最后在 main 里把整条链点起来,顺序是死的:

```cpp
cinux::lib::kprintf_init();
cinux::arch::g_gdt.init();   // ① GDT 先
cinux::arch::g_idt.init();   // ② IDT 后(它的 gate 引用 GDT 选择子)
__asm__ volatile("int $3");  // ③ 故意触发断点,验证整条链通不通
```

IDT 的 gate 里填的 selector 是 `GDT_KERNEL_CODE`,GDT 没加载之前这个选择子是无效的,先 init IDT 就等于埋了颗雷,等 `int $3` 一进来就炸。

## 调试现场

这一段本该放真实的踩坑笔记,但 010 这个 tag 留下的 notes 是空的。好在源码注释里藏着一句非常实在的话,我们直接拿来用——main 在 `int $3` 之前有这么一句提醒:

> Note: do NOT enable interrupts (sti) yet -- we have no IRQ handlers and a pending PIT timer would cause a Double Fault via unhandled IRQ.

翻译过来就是:现在千万别 sti。我们还没有任何 IRQ handler,可一旦 sti,PIT 定时器的中断就会到来,没人接它,结果就是 Double Fault,直接重启。

这句话重要,不只因为它本身是个坑——它正好点明了这一章的边界。010 装的是"异常"安全网,接的是 CPU 自己抛的 #BP、#PF;可"硬件中断"(时钟、键盘)是另一回事,那需要 8259 PIC 和一套 IRQ handler,是下一个 tag 的活。所以这里不 sti 不是疏忽,是刻意的:在 IRQ 体系建好之前,sti 就是自找麻烦。

## 验证

`make run-big-kernel-test` 会在 QEMU 里把这条链从头验到尾:它检查 `int $3` 之后内核是不是真的活着继续、连续触发多次异常状态会不会腐坏、还有 gate 编码 `0x8E`/`0xEF` 算得对不对。

手动看效果,`make run` 的串口大概是这个样子:

```text
[BIG] GDT loaded.
[BIG] IDT loaded.
[BIG] Triggering int $3 breakpoint...

==== EXCEPTION: #BP (vector 3) ====
  RIP   = 0x...        CS  = 0x0008
  RFLAGS= 0x...
  ...
[EXCEPTION] Breakpoint at RIP=0x...
[EXCEPTION] Continuing...
[BIG] Breakpoint returned, continuing.
```

最后那行 `Breakpoint returned, continuing.` 就是我们要的点:异常被接住了,现场 dump 出来了,内核还活着。

## 下一站

到这里,big kernel 第一次有了扛事的能力——CPU 抛异常,我们能接住、能看到现场、能活着回来。但你大概也注意到了,全程没碰 sti,没碰任何硬件中断。这意味着内核现在还是"聋"的:听不到时钟、听不到键盘,外部世界发生什么它都不知道。

打破这个静默的,是下一站 011:配上 8259 PIC、挂上 PIT 定时器、写好 IRQ handler,然后才敢 `sti`。到那时,main 里那句"现在不能 sti"的警告,才终于可以解禁。

---

### 参考

- Intel SDM Vol.3A — §6.10 + Figure 6-2/6-8(IDT gate descriptor)、Table 6-1(异常向量分配)、Figure 6-11 / Table 35-14(#PF error code)、CR2 说明。interrupt gate=0xE、trap gate=0xF 及其对 IF 的处理均为 SDM 明文标准。
- OSDev — Interrupt Descriptor Table、Exceptions。
- 本 tag 源码:[idt.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/idt.hpp)、[idt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/idt.cpp)、[interrupts.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/interrupts.S)、[exception_handlers.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/exception_handlers.cpp)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp);测试 [test_gdt_idt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_gdt_idt.cpp)。
