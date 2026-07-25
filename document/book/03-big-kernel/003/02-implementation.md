---
title: 02 · 代码路线:scoped enum、gate 策略与 ISR stub
---

# 代码路线:scoped enum、gate 策略与 ISR stub

## 用 scoped enum 把异常和 gate 类型说人话

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

## gate 策略:为什么 #BP 用陷阱门,其余用中断门

这里有个设计决策值得展开讲。你看异常表会发现,大部分异常我们挂的是中断门(Interrupt, 0xE),唯独 #BP 和 #DB 挂了陷阱门(Trap, 0xF),这不是随便选的。

差别在一个标志位上,IF,也就是中断使能标志。CPU 进入中断门时会自动把 IF 清掉,意思是 handler 执行期间不再响应新的可屏蔽中断,得等它跑完 `iretq` 才恢复;而陷阱门不清 IF,中断状态原样保留。

对大部分致命异常(#PF、#GP、#DF 这类)来说,我们进 handler 就是去打印现场然后死给你看的,期间当然不希望再被别的东西打断,所以用中断门顺手清掉 IF 是对的。但 #BP 和 #DB 不一样,它们是非致命的——一个断点打完,内核要活着继续跑。要是它俩也清 IF,万一将来我们在中断已经使能的状态下命中断点,IF 就被悄悄改掉了,行为会变得很难解释。所以给它们用陷阱门,保证打完断点回来,中断状态和进去之前一模一样。这种"致命用中断门、诊断用陷阱门"的区分,是这版 IDT 里最值得记住的一笔。

## 数据驱动的路由表

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

## ISR stub:汇编里的两个宏

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

## C handler:致命的躺平,非致命的继续

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

## 串起来:main 里的顺序,和那句要命的注释

最后在 main 里把整条链点起来,顺序是死的:

```cpp
cinux::lib::kprintf_init();
cinux::arch::g_gdt.init();   // ① GDT 先
cinux::arch::g_idt.init();   // ② IDT 后(它的 gate 引用 GDT 选择子)
__asm__ volatile("int $3");  // ③ 故意触发断点,验证整条链通不通
```

IDT 的 gate 里填的 selector 是 `GDT_KERNEL_CODE`,GDT 没加载之前这个选择子是无效的,先 init IDT 就等于埋了颗雷,等 `int $3` 一进来就炸。
