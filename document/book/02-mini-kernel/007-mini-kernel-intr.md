---
title: 007 · 第一次能被打断
---

# 007 · 第一次能被打断:内核的 GDT、IDT 与异常处理

> 从 [002](../01-boot/002-boot-gdt-protected.md)/[003](../01-boot/003-boot-long-mode.md) 一路到现在,我们的内核一直有个尴尬的限制:它全程 `cli`,关着中断。这意味着任何 CPU 异常——哪怕是调试时最常用的 `int $3` 断点——一来就没人接,CPU 直接三重故障重启。这一章,我们给 mini kernel 装上它自己的 **GDT** 和第一张 **IDT**,写出异常处理 stub,让它第一次能"被打断"、看一眼出了什么事、然后活着继续。

## 这一章我们要点亮什么

目标具体而实在:让两条异常被正确接住。第一条是 #BP(向量 3)断点异常,执行 `int $3` 触发——我们要让它的处理程序把当时的寄存器全部 dump 到串口,然后返回继续执行,因为 `int $3` 是陷阱,RIP 指向下一条,返回后内核能接着跑。第二条是 #PF(向量 14)页错误——处理程序从 `CR2` 读出导致缺页的地址、解析错误码的各个位(读还是写、不存在还是权限冲突……),打印出来,然后也继续。

为此要搭三件东西:一张内核自己的 GDT(3 项:空、64 位代码、64 位数据)、一张 IDT(256 项里只填 #BP 和 #PF 两个向量)、以及把异常导流到 C 函数的 ISR stub(汇编)。

做完之后,`make run` 的串口上会看到 `[INIT] GDT loaded`、`[INIT] IDT loaded`,然后内核主动 `int $3`,屏幕打出一段 `==== EXCEPTION: #BP (vector 3) ====` 带着一堆寄存器,最后 `[TEST] Breakpoint test passed! Execution continued after #BP.`——最后这句"继续执行了"就是整个系统的通过信号:异常被接住、内核没死。

## 为什么现在需要它

`cli` 这道闸,在 boot 阶段是保护伞(没有 IDT 时开中断必崩),但到了内核阶段就成了枷锁。一个永远不被打断、一遇异常就重启的内核,没法调试,也没法往上堆任何需要异步的东西(时钟、键盘、系统调用)。

而要"能被打断",内核得先有自己的 IDT——告诉 CPU"向量 3 的异常去这个地址处理、向量 14 去那个地址"。但 IDT 里每个门都要填一个**代码段选择子**,指向 GDT 里的代码段;bootloader 在 003 建过一张临时 GDT 把我们送进长模式,那张表是"用完即弃"的,内核现在要换上自己的 GDT,IDT 才有可靠的选择子可指。所以顺序是死的:**GDT 先,IDT 后**。

这一章只配两个异常向量,不碰 PIC、不碰硬件中断。注意 `int $3` 是一条**同步**指令——它立刻触发 #BP,不需要"开中断"(`sti`)。事实上这一章全程不开 `sti`:我们还没有可编程中断控制器(PIC)的驱动,贸然 `sti` 让定时器中断进来却没人接,当场三重故障。所以这里能演示的是"同步触发的异常被接住",硬件中断要等后面接上 PIC 之后。

> 外部依据:OSDev 的 Interrupt Descriptor Table / Exceptions 页描述了 64 位 IDT 门描述符格式、#BP/#PF 的语义与错误码各位含义;Intel SDM 规定了中断门(进入时清 IF)与陷阱门(不清 IF)的区别。

## 设计图

先看这三张表怎么协作。一次 `int $3` 的完整旅程:

```text
int $3 指令
   │  CPU 查 IDT[3] → 得到 isr_bp_stub 地址 + 代码段选择子
   ▼
isr_bp_stub (汇编)
   ├─ push $0              ← 补一个伪错误码(#BP 没有硬件错误码,补 0 保持栈帧统一)
   ├─ push rax, rbx, ... r15   ← 保存全部通用寄存器(构造 InterruptFrame)
   ├─ mov %rsp, %rdi       ← InterruptFrame* 作为第一参数
   ├─ call handle_bp       ← 进 C
   │     └─ dump 寄存器到串口,返回
   ├─ pop r15..rax         ← 恢复寄存器
   ├─ add $8, %rsp         ← 跳过那个伪错误码
   └─ iretq                ← 返回被中断的代码,继续往下跑
```

再看 IDT 里一个"门"长什么样(16 字节),关键字段是处理程序地址(拆三段存)和 `type_attr`:

```text
IdtEntry (16 字节)
  offset_low [0:15] ┐
  offset_mid [16:31]├─ 处理程序地址(64 位,拆三段)
  offset_high[32:63]┘
  selector          ← 代码段选择子(指 GDT 的 code64 = 0x08)
  ist               ← IST 偏移(本章用 0)
  type_attr         ← P|DPL|0|GateType
  reserved
```

`type_attr` 这一个字节决定门的关键行为,本章用两个值:#BP 用 `0x8F`(陷阱门),#PF 用 `0x8E`(中断门),差别下面讲。

## 代码路线

### 1. 内核自己的 GDT:换掉 bootloader 的临时表

[gdt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/arch/x86_64/gdt.cpp) 建一张三项的扁平 GDT——空段、64 位代码段、64 位数据段,和 003 进长模式那张是同一种"扁平模型",只是现在由内核自己拥有:

```cpp
s_gdt[GDT_NULL_INDEX]  = make_gdt_entry(0, 0,       0,    0);     // 空
s_gdt[GDT_CODE64_INDEX]= make_gdt_entry(0, 0xFFFFF, 0x9A, 0x0A);   // 代码:access 0x9A, flags 0x0A(L=1)
s_gdt[GDT_DATA64_INDEX]= make_gdt_entry(0, 0xFFFFF, 0x92, 0x0C);   // 数据:access 0x92, flags 0x0C
```

`0x9A` 是"present + ring0 + 代码段 + 可读",`0x0A` 的高位 flags 是 `G=1, D=0, L=1`(L 位=1 才是 64 位代码段);数据段把"可执行"去掉得 `0x92`。base 全 0、limit 全 1——扁平,段透明。选择子 `SEGMENT_CODE64 = 1*8 = 0x08`、`SEGMENT_DATA64 = 2*8 = 0x10`。

`gdt_init` 把表填好后,`lgdt` 加载,然后用 002 那套"压目标 CS + 压返回地址 + `lretq`"的远返回把 `CS` 切到 `0x08`,再 `mov` 重载 `DS/ES/FS/GS/SS`。这套刷新流程 002 讲过原因(长模式不能 `mov cs`),这里只是内核在自己的地址空间里重做一遍。

### 2. IDT:给异常一个"入口地址表"

GDT 就位,IDT 才有可靠选择子可用。[idt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/arch/x86_64/idt.cpp) 先把 256 项清空(`Present=0` 表示未用),再只填我们关心的两个向量:

```cpp
set_idt_entry(IDT_VEC_BP, isr_bp_stub, SEGMENT_CODE64, 0x8F, 0);  // #BP 陷阱门
set_idt_entry(IDT_VEC_PF, isr_pf_stub, SEGMENT_CODE64, 0x8E, 0);  // #PF 中断门
```

`set_idt_entry` 把处理程序地址(这里是汇编 stub `isr_bp_stub` 的地址)拆成低/中/高三段填进 `IdtEntry`,记下选择子 `0x08`、IST=0、`type_attr`。填完 `lidt` 加载,IDT 就生效了。

注意填进去的是**汇编 stub 的地址**,不是 C 函数 `handle_bp` 的地址。CPU 进异常时,栈上的状态是固定的(错误码、rip、cs、rflags、rsp、ss),和 C 的调用约定(参数走 `rdi`)对不上,不能让 CPU 直接跳进 C 函数——中间必须有一段汇编 stub 做适配。这就是下一节。

### 3. ISR stub:保存现场、传 InterruptFrame*、iretq 回去

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

### 4. 伪错误码:#BP 为什么要在栈上补一个 0

为什么要给 #BP 这个"没有错误码"的异常硬塞一个 0?为了让**两种异常的栈帧布局统一**。

有错误码的异常(像 #PF),CPU 进入时栈上是 `错误码 / rip / cs / rflags / rsp / ss`;没错误码的(像 #BP),栈上直接是 `rip / cs / rflags / rsp / ss`,少了一项。如果 stub 不补,那么 `handle_bp` 拿到的 `InterruptFrame*` 和 `handle_pf` 拿到的,字段就对不齐——同样是 `frame->error_code`,一个读到的是真错误码、另一个读到的其实是 rip。补一个伪 0 之后,两种异常的 `InterruptFrame` 布局完全一致,C 处理函数不用区分对待。stub 末尾 `addq $8` 跳过它,刚好抵消。

### 5. C handler:#BP 继续,#PF 读 CR2 + 解错误码

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

### 6. 陷阱门 vs 中断门:0x8F 与 0x8E 的区别

回到 IDT 那个 `type_attr` 字节。`0x8F` 和 `0x8E` 只差最后一位:门类型 `0xF`(陷阱门)vs `0xE`(中断门)。`0x8` 的高位是 `Present=1, DPL=0`。

这一位之差,决定 CPU 进入处理程序时**要不要清 IF(中断允许标志)**。中断门(`0xE`)进入时 CPU 自动清 IF,整个处理过程不再响应其它中断,出来 `iretq` 时再恢复;陷阱门(`0xF`)不清 IF,处理期间仍可被(更高优先级的)中断打断。

Cinux 的选择是:#BP 用陷阱门 `0x8F`(断点处理时允许嵌套),#PF 用中断门 `0x8E`(页错误处理时不希望被打断,把 IF 清掉更稳)。这种"故障/错误类用中断门、调试/软中断类用陷阱门"的搭配是常见做法。

顺带纠一个源码注释的笔误:`0x8F` 高位的 `8` 表示 `Present=1, DPL=0`,所以这个 #BP 门其实只有 ring0 能触发。但 [idt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/arch/x86_64/idt.cpp) 那行注释写着"DPL=3 允许用户态 INT3"——那是不对的,要真让用户态 `int $3`,得把 DPL 设成 3,也就是用 `0xEF`。当前 mini kernel 只有 ring0,这个差别无所谓,但别被那条注释带偏。

## 调试现场

这一章没有 notes,但中断这块的坑很典型,从机制本身就能预见到几个。

头号坑是"忘了初始化 IDT 就触发异常"。症状是三重故障重启,连 dump 都没有。根因是 IDT 还没 `lidt`(或根本没建),CPU 查 `IDT[3]` 查到一个 `Present=0` 的空项,直接认为"没处理器"→ #GP → 又没 #GP 处理器 → 三重故障。修复就是 `gdt_init` → `idt_init` 必须在任何异常触发之前。判断:确认串口上有没有 `[INIT] IDT loaded` 这行,没有就是漏了。

第二个是 ISR stub 压栈顺序写反。症状是 dump 出来的寄存器"错位"(比如 RAX 显示的是 RBX 的值)。根因是 stub 的 push 顺序和 `InterruptFrame` 字段顺序对不上。修复:严格按"rax 先压、r15 最后压"配 `InterruptFrame` 从 r15 起的字段定义,且压入和弹出顺序严格相反。

第三个是门类型选错。#PF 误用陷阱门,处理期间 IF 没清,如果之后接了 PIC,一个嵌套中断进来可能把栈搞乱。这种 bug 现阶段不爆(还没 `sti`),但埋着。对照"故障用中断门、陷阱用陷阱门"的原则核一遍。

第四个是 #PF 错误码读反。`frame->error_code` 各位含义记反(比如把 bit1 的读写弄反),dump 出来的"读/写"就是反的,误导排查。对着 Intel SDM 的 #PF 错误码位定义核。

## 验证

这一章有两套验证,各管一层。

host 单测验"编码对不对":

```bash
cmake --build build --target test_host
```

`test_gdt_idt`(六百多行)专门测 GDT/IDT 的字节级编码——`make_gdt_entry` 出来的描述符字节对不对、gate 的 `type_attr` 算对没、选择子值对不对。这些是纯逻辑(给定 access/flags,算出 8 字节描述符),适合 host 磨,不用启动 QEMU。

QEMU 内核测验"真能跑":

```bash
cmake --build build --target run-kernel-test
```

`test_interrupts` 会真触发异常,确认 handler 跑了、寄存器 dump 出来、内核存活。

量产内核看效果:

```bash
cmake --build build --target run
```

串口依次出现 `[INIT] Setting up GDT...`、`GDT loaded`、`Setting up IDT...`、`IDT loaded`,PMM 统计,然后 `[TEST] Triggering breakpoint exception (int $3)...`,紧跟着 `==== EXCEPTION: #BP (vector 3) ====` 和一整块寄存器,最后 `[TEST] Breakpoint test passed! Execution continued after #BP.`。看到"continued after #BP",说明异常被接住、`iretq` 正确返回、内核活着。

## 下一站

mini kernel 现在有了内存(PMM)、会说话(串口/kprintf)、还能被异常打断并自愈。可它终究是个"mini"——它是为引导一个**更大的内核**做准备的跳板。那个真正完整的 big kernel(有完整 GDT/TSS、有驱动、有进程)还没登场。

下一章 [008 · 加载大内核](008-load-large-kernel.md),mini kernel 要从磁盘把那个 big kernel 的 ELF 镜像读进来、解析、加载到该去的位置,然后把控制权交过去。mini kernel 由此完成它"从 bootloader 手里接力、再把棒交给 big kernel"的全部使命。从那以后,Cinux 的主角就换成 big kernel 了。

---

### 参考

- OSDev — [Interrupt Descriptor Table](https://wiki.osdev.org/Interrupt_Descriptor_Table)(64 位门描述符格式)、[Exceptions](https://wiki.osdev.org/Exceptions)(#BP/#PF 语义与错误码各位)、[Interrupt Service Routines](https://wiki.osdev.org/Interrupt_Service_Routines)(ISR stub 栈帧约定)。
- Intel SDM Vol.3A — IDT 门描述符(中断门/陷阱门与 IF 行为)、#BP/#PF 异常、CR2、页错误码位定义。(章节号以本地 2023-06 版标题为准。)
- 本 tag 源码:[gdt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/arch/x86_64/gdt.cpp)/[gdt.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/arch/x86_64/gdt.hpp)、[idt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/arch/x86_64/idt.cpp)/[idt.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/arch/x86_64/idt.hpp)、[interrupts.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/arch/x86_64/interrupts.S)、[exception_handlers.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/arch/x86_64/exception_handlers.cpp)、[test_gdt_idt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_gdt_idt.cpp)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/main.cpp)。

> 参考 URL 的有效性会在全局审查阶段用 open-websearch(bing)统一核活。
