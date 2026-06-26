---
title: 02 · 寻址、远转移与系统指令
---

# 02 · 寻址、远转移与系统指令:AT&T 里那些最劝退的写法

> 上一章我们把"源在左、目的在右、寄存器加 `%`、立即数加 `$`"这套基本约定立住了。这一章专攻三块视觉差异最大、最容易让 NASM 老手和 C 程序员一起愣住的写法:**内存寻址的 `disp(base,index,scale)`、段前缀、以及一类只在内核里出现的"系统指令"**(CR/MSR/lgdt/iretq)。读完你能直接看懂正文 boot 和 kernel 里的汇编。

> 本章只讲**汇编形态**——一条指令长得什么样、AT&T 怎么写。至于 CR0/CR3/CR4 每一位的语义、MSR 各地址含义、iretq 弹栈的特权级检查,正文 001–003 会逐位讲透,这里只给跳转。

## 先把"三种操作数"一眼分清

在写任何一行汇编前,先把 AT&T 怎么区分三种操作数钉死。这是后面所有写法的根:

```text
操作数样子      它是什么        AT&T 写法举例(以"读进 %eax"为例)
$0x1234        立即数(常数)    movl $0x1234, %eax     ← 数值本身
%ebx           寄存器          movl %ebx, %eax        ← 寄存器里的值
8(%ebx)        内存(带偏移)    movl 8(%ebx), %eax     ← 内存地址处的值
```

三条铁律,记牢:

- **立即数前面必须有 `$`**。漏了 `$`,`movl 0x1234, %eax` 会被当成"从绝对地址 `0x1234` 处取 4 字节",语义整个变了——这是新手最常见的"我明明想写常数怎么读了一坨内存"。
- **寄存器前面必须有 `%`**。`movl ebx, eax` 在 AT&T 里既不是寄存器也不是合法符号,直接汇编报错。
- **内存操作数没有任何前缀**(不像 NASM 的 `[ebx]`)。它的特征是**带小括号**:`(base)`、`disp(base)`、`disp(base,index,scale)`。一个数后面跟个括号,就是"这是地址"。

Cinux 里随手就能找到三种齐全的例子。看 `boot/common/long_mode.S` 进长模式那几行:

```asm
movl $PML4_PHYS_ADDR, %eax   // $PML4_PHYS_ADDR:立即数(符号常量)进 eax
movl %eax, %cr3              // %cr3:寄存器(控制寄存器也算寄存器,照样 %)
movl %eax, PML4_PHYS_ADDR    // PML4_PHYS_ADDR:绝对地址(无括号、无 %)→ 写内存
```

第一条 `$PML4_PHYS_ADDR` 是 `.set` 定义的常量,等于一个立即数 `0x1000`;第二条 `%cr3` 是控制寄存器;第三条 `PML4_PHYS_ADDR` 不带 `$` 也不带 `%`,汇编器把它当"以这个值为地址的内存"——等价于 Intel 的 `mov [0x1000], eax`。同一个名字,加不加 `$` 含义天差地别,这是 AT&T 最"坑"也最一致的地方。

> 外部依据:GCC 在线手册 *18.1 AT&T Syntax* 节明确写明"immediate operands are preceded by `$`,registers are preceded by `%`,and a bare symbol without either is treated as a memory reference"。

## 内存寻址:`disp(base, index, scale)`

实模式下我们用 `DS:SI`、`ES:DI` 这种"段:偏移"访问内存。到了 32/64 位,内存地址统一写成下面这个**通用形式**:

```text
AT&T:   disp(base, index, scale)
Intel:  [base + index*scale + disp]
```

- `base`、`index` 是两个通用寄存器(可省略其一);
- `scale` 只能是 `1`、`2`、`4`、`8`(数组元素大小);
- `disp` 是一个常量偏移,可正可负、可省略。

这套写法直接对应 x86 的 ModR/M+SIB 编码能力,**它不是 GAS 的发明,是 CPU 硬件就这么寻址**。AT&T 只是用 `()` 把它包起来,Intel 用 `[]`。

看 Cinux 真实用法。`boot/stage2.S` 里 MBR 给 DAP 填字段,全是 `disp(base)`:

```asm
movw $DAP_STORE_ADDR, %si       // si 指向 DAP 结构基址
movb $DAP_SIZE, (%si)           // (%si)        → disp=0, 写 DAP[0]
movw $STAGE2_SECTORS, 2(%si)    // 2(%si)       → disp=2, 写 DAP[2]
movl $STAGE2_LBA, 8(%si)        // 8(%si)       → disp=8, 写 DAP[8..11]
```

`(%si)` 就是"以 `si` 的值为地址",`2(%si)` 就是"以 `si+2` 为地址"。这是**一基址 + 一偏移**的最常用形态,等价于 C 里的 `struct->field`:基址寄存器当结构体指针,`disp` 当字段偏移。

`kernel/arch/x86_64/context_switch.S` 里则把"存上下文"写成连续的 `disp(%rdi)`,`%rdi` 指向 `CpuContext`:

```asm
movq %r15, 0(%rdi)              // from+0:  存 r15
movq %r14, 8(%rdi)              // from+8:  存 r14
movq %r13, 16(%rdi)             // from+16: 存 r13
```

> 详见正文 **001 · 实模式引导** 的 DAP 字节布局;`CpuContext` 的字段偏移见正文进程卷。

**完整三件套**(带 index 和 scale)用来遍历数组。`boot/stage2.S` 里把内存映射条目逐字节搬运就用上了:

```asm
movb (%rsi, %rcx), %al          // 读 src[index]   = base=%rsi, index=%rcx, scale=1
movb %al, (%rdi, %rdx)          // 写 dst[index]   = base=%rdi, index=%rdx, scale=1
```

`(%rsi, %rcx)` —— base 是 `%rsi`,index 是 `%rcx`,scale 省略默认 1,disp 省略默认 0。等价于 Intel 的 `[rsi + rcx]`。如果数组元素是 4 字节,就写成 `(%rsi, %rcx, 4)`。

> 外部依据:这套 `disp(base,index,scale)` 通用寻址格式在 Intel SDM Vol.2A 的 *3.1.1.3 Base + Displacement / Base + Index × Scale + Displacement* 小节有完整描述(被称为 SIB 寻址);OSDev 的 [Memory Addressing](https://wiki.osdev.org/Memory_Addressing) 页有社区版总结。

## 段前缀:`%es:(%di)` / `%gs:`

寻址默认走 `DS` 段(`(%si)` 其实是 `DS:SI`)。想从**别的段**读写,就给内存操作数加一个**段前缀**。AT&T 里段前缀写在操作数内部、紧跟一个冒号:

```asm
movw $0x4256, %es:(%di)         // 写到 ES:DI,而不是 DS:DI
movw $0x3245, %es:2(%di)        // 写到 ES:(DI+2)
```

这是 `boot/common/serial.S` 里给 VBE 信息块写签名 `"VBE2"` 的真实两行。`%es:` 紧贴在 `(%di)` 前面(注意 `%es:(%di)` 里那个冒号——它和偏移 `2(%di)` 是两层不同的东西:段前缀管"用哪个段",`2()` 管"段内偏移多少")。

读的时候也一样,`vesa_save_framebuffer_info` 里从 BIOS 写好的 ModeInfoBlock(在 `ES` 段)读物理地址:

```asm
movl %es:MODE_PHYS_BASE_PTR(%di), %eax   // 从 ES:(DI+0x28) 读 4 字节到 eax
movl %eax, %gs:FB_INFO_PHYS_ADDR(%di)    // 写到 GS:(DI+0)
```

同一行里**源操作数带 `%es:`、目的操作数带 `%gs:`**,两个段不同,这就是"跨段搬运"。64 位长模式下基本不用分段了,但 `%fs:`/`gs:` 这两个段前缀在内核里另有用途(线程本地存储 TLS、per-CPU 数据),所以**形式你得认得**,语义见正文。

> 详见正文 **001** 的 VESA framebuffer 存档;长模式下 `%gs:` 的 per-CPU 用法见正文中断/进程卷。

## 远转移:`ljmp` / `lcall`

普通 `jmp` 只改 `RIP`,**远转移**同时改 `CS`(代码段选择子)和 `RIP`。只要涉及**模式切换**(实→保护→长),CS 必须换,就得用远转移。AT&T 写法:

```asm
ljmp $segment, $offset          // ljmp:远跳,立即数段选择子 + 立即数偏移
```

注意**两个操作数都是立即数**,所以都带 `$`。`boot/mbr.S` 第一条指令就把 CS 钉死:

```asm
ljmp $0, $real_start            // CS=0, RIP=real_start(段归一化)
```

`$0` 是段选择子(实模式下就是段值 0),`$real_start` 是标号地址。正文 001 讲过为什么要这一跳:BIOS 可能用 `0x07C0:0x0000` 进来,先把 CS 强制成 0,后面所有按 CS 算地址的代码才确定。

模式切换时,段选择子换成 GDT 里的描述符索引:

```asm
// boot/stage2.S:实模式 → 32 位保护模式
movl %eax, %cr0                 // 先打开 CR0 的 PE 位
ljmp $0x08, $pm_entry           // CS=0x08(GDT 里 32 位代码段),跳进保护模式

// boot/common/long_mode.S:保护模式 → 64 位长模式
ljmp $0x18, $long_mode_entry    // CS=0x18(GDT 里 64 位代码段),跳进长模式
```

`$0x08`、`$0x18` 这些数是 GDT 选择子(描述符在表里的偏移),正文 002/003 会讲怎么算。**本章只认形:`ljmp $选择子, $标号`,且因为换 CS,所以必须紧跟在改 CR0/CR4/EFER 之后**——换段的那一下,就是把 CPU 推进新模式的那一下。

> 详见正文 **002 · GDT 与保护模式**、**003 · 进入长模式**:CR0.PE / EFER.LME / CR0.PG 各位的语义、GDT 选择子的位域、为什么换 CS 等于真正"进入"新模式。

`lcall`(远调用)形态完全一样,只是它压栈返回的 `CS:RIP` 而不是直接跳。Cinux bootloader 里几乎不用,知道有这东西即可。

## 系统指令:CR / MSR / lgdt 的 AT&T 形态

这一节是本章重点,也是"看着像外星文"的高发区。但这些指令在 AT&T 里**反而比 Intel 干净**——控制寄存器和 MSR 不是普通内存,它们的读写就两个固定套路。

### 控制寄存器(CR0/CR3/CR4):当寄存器写,但用 `mov`

控制寄存器在 AT&T 里**就是带 `%` 的寄存器**:`%cr0`、`%cr3`、`%cr4`。读写都用普通 `mov`,只是源/目的是 `%crN`:

```asm
// boot/stage2.S:打开 CR0 的 PE 位(保护模式使能)
movl %cr0, %eax                 // 读 CR0 → eax
orb  $0x1, %al                  // 只改 bit0(PE 位)
movl %eax, %cr0                 // 写回 CR0
```

进长模式时还要写 CR3(页表基址)和 CR4(开 PAE):

```asm
// boot/common/long_mode.S
movl %eax, %cr3                 // 写 CR3:加载 PML4 页表物理基址
movl %cr4, %eax
orl  $CR4_PAE, %eax             // 置 CR4 的 PAE 位(bit5)
movl %eax, %cr4
```

形很简单。**难点全在"CR0/CR3/CR4 每一位管什么"——那是正文 002/003 的活,本章不展开**,你只要知道"在 AT&T 里,控制寄存器长这样、用 `movl` 读写"。

### MSR:rdmsr / wrmsr 与"高低 32 位拆写"

**这是最容易劝退、但必须讲透的一点**。MSR(Model-Specific Register)是 64 位的,但 `rdmsr`/`wrmsr` 这两条指令**在 32 位兼容语义下工作**:它们固定用一对 32 位寄存器 `EDX:EAX` 来装那 64 位值,高 32 位在 `%edx`、低 32 位在 `%eax`。**没有"一条指令直接读进 64 位 `%rax`"这种写法**。

标准套路四步:

```asm
movl $0xC0000080, %ecx          // ① ECX = MSR 地址(指定读哪个 MSR)
rdmsr                            // ② 读:EDX:EAX = MSR[ECX]
// 现在 %edx = 高 32 位,%eax = 低 32 位
```

写反过来:

```asm
movl $0xC0000080, %ecx          // ① ECX = MSR 地址
// 先把想写的值分到 %edx(高 32)、%eax(低 32)
wrmsr                            // ④ 写:MSR[ECX] = EDX:EAX
```

为什么要拆?因为 `rdmsr`/`wrmsr` 的 ISA 定义就是 32 位对、靠 `ECX` 选址、靠 `EDX:EAX` 传值——这是硬件定的,不是 GAS 的选择。`boot/common/long_mode.S` 开 EFER.LME 就是这个套路:

```asm
movl $MSR_EFER, %ecx            // ECX = 0xC0000080(EFER 地址)
rdmsr                            // 读 EFER → EDX:EAX
orl  $EFER_LME, %eax             // 只改 eax 的 bit8(LME),edx 高位不动
wrmsr                            // 写回 EFER = EDX:EAX
```

**拆写的典型场景:把 64 位 MSR 值存进内存结构**。`kernel/arch/x86_64/context_switch.S` 保存 `MSR_GS_BASE`(0xC0000101)就是教科书例子——它不能一条 `movq` 落地,只能**拆成两条 `movl`,各写 4 字节**:

```asm
movq $0xC0000101, %rcx          // ECX = MSR_GS_BASE 地址
rdmsr                            // EDX:EAX = GS_BASE 的 64 位值
movl %eax, 64(%rdi)             // from+64: 写低 32 位(EDX:EAX 里的 EAX)
movl %edx, 68(%rdi)             // from+68: 写高 32 位(EDX:EAX 里的 EDX)
```

`%eax` 写到偏移 64、`%edx` 写到偏移 68——连续 8 字节正好拼成一个 64 位字段。恢复时完全镜像:从内存读两半、塞回 `%eax`/`%edx`、再 `wrmsr`:

```asm
movl 64(%rsi), %eax             // 读低 32 位 → EAX
movl 68(%rsi), %edx             // 读高 32 位 → EDX
movq $0xC0000101, %rcx
wrmsr                            // 写回 GS_BASE
```

这就是任务里说的"`movl %eax,64(%rdi)` / `movl %edx,68(%rdi)` 这类高低 32 位拆写"。**记一句话:凡是 64 位 MSR 要和内存互搬,在 AT&T 里永远是两条 `movl`,低半跟着 `%eax`、高半跟着 `%edx`**。

> 外部依据:Intel SDM Vol.2B *RDMSR—Read From Model Specific Register* / *WRMSR—Write* 描述了 `ECX` 选址、`EDX:EAX` 传 64 位值、操作数大小固定 32 位的语义;MSR 地址清单(如 `MSR_GS_BASE=0xC0000101`、`MSR_EFER=0xC0000080`)在 Vol.4 *Model-Specific Registers* 逐条列出。本仓库 `document/reference/intel/SDM-Vol4-Model-Specific-Registers.pdf` 即这一卷。

### lgdt:把 GDT 指针塞进 GDTR

`lgdt` 读取一个 6 字节的"GDT 指针"(2 字节 limit + 4/8 字节 base)装进 `GDTR` 寄存器。它的操作数是**一个内存地址**——AT&T 里就是一个内存操作数:

```asm
lgdt gdt_ptr                    // 从符号 gdt_ptr 指向的 6 字节加载 GDTR
```

`gdt_ptr` 是个裸符号(不带 `$`、不带 `%`),所以是"以该符号为地址的内存"。`boot/stage2.S` 进保护模式、`boot/common/long_mode.S` 进长模式前各 `lgdt` 一次。**形式就是 `lgdt 内存操作数`,注意是读内存、不是立即数**——别写成 `lgdt $gdt_ptr`。

## iretq、压栈序列与 16 字节对齐

中断返回 `iretq`(64 位版,带 `q` 后缀)从栈上弹出 `RIP`、`CS`、`RFLAGS`、`RSP`、`SS` 共 5 个 8 字节(40 字节),把 CPU 恢复到中断前的状态。ISR(中断服务例程)的典型骨架,看 `kernel/arch/x86_64/interrupts.S`:

```asm
pushq $0                        // 没有硬件错误码的异常:塞个假错误码,保证栈布局统一
pushq %rax                      // 压全部通用寄存器,在栈上拼出一个 InterruptFrame
pushq %rbx
...                              // 共 push 15 个通用寄存器
pushq %r15
pushq $0                        // 对齐填充:把上面的总数凑成 16 的倍数(见下文)

leaq 8(%rsp), %rdi              // 跳过对齐填充,%rdi 指向栈上寄存器区 → 传给 C handler
call \handler                    // 调 C 写的中断处理函数

addq $8, %rsp                   // 丢掉对齐填充
popq %r15                       // 逆序弹回全部寄存器
...
popq %rax
addq $8, %rsp                   // 丢掉(假)错误码
iretq                            // 弹 RIP/CS/RFLAGS/RSP/SS,返回中断点
```

这段里有三个 AT&T/ABI 细节,都是内核人必须知道的:

**1. `push`/`pop` 是单操作数,但 AT&T 照样要 `%`。** `pushq %rax`、`popq %r15`,寄存器加 `%`,没有"源/目的"之分(它的隐含操作数是栈顶 `(%rsp)`)。

**2. `call` 的目标是个标号或 `*` 间接。** `call \handler` 是宏参数展开成的标号;间接调用要加 `*`,如 `jmp *%rax`(跳到 `rax` 里的地址)、`jmp *56(%rsi)`(跳到 `to+56` 里存的 RIP)。这个 `*` 是 AT&T 的"间接跳转"标记,NASM 里是 `jmp rax` 直接写。

**3. 16 字节栈对齐——这是 64 位最隐蔽的坑。** System V AMD64 ABI 规定:**进入一个普通函数时,`RSP % 16 == 8`**(因为 `call` 已经压了 8 字节返回地址,从一个 16 对齐的点变成 `...8`)。中断打断的瞬间,CPU 自动压 5 个 8 字节(SS/RSP/RFLAGS/CS/RIP,40 字节)+ 可能的错误码——这些不是 16 的倍数,所以 ISR 进来时栈**没对齐**。直接 `call` 一个 C 函数,C 函数里一旦用 SSE/`movaps` 这种要求 16 字节对齐的指令,当场 `#GP` 崩。

Cinux 的修法在注释里写得很清楚(interrupts.S 第 56–63 行):压完 15 个 GPR 后,再 `pushq $0` 塞一个 8 字节对齐填充,把总数凑成 16 的倍数,这样紧跟着的 `call` 落下去,C handler 入口正好满足 `RSP % 16 == 8`。**64 位内核写 ISR,这条对齐填充省不得**。

> 详见正文 **003 / 中断卷**:中断向量、IDT 表项格式、特权级切换、`iretq` 弹栈的特权级检查与栈切换细节。

## 64 位小禁忌:`pusha`/`popa` 别用

最后一条,够短但够致命。`pusha`/`popa`(一次压/弹 8 个通用寄存器)在 16/32 位很好用,但 **64 位模式下这两条指令无效(`#UD` 非法指令异常)**。所以 `interrupts.S` 里你看到的是老老实实把 15 个寄存器一个一个 `pushq %rax`、`pushq %rbx` ……——不是 Cinux 作者啰嗦,是 64 位只能这么干。如果你从 32 位教程抄了 `pusha` 过来,一进长模式就 `#UD` 重启,排查很久。

同样,`push`/`pop` 在 64 位模式下有个坑:**不能用 32 位操作数大小**——`push %eax`、`push $imm32` 这类在 64 位下根本无法编码。16 位的 `push %ax` 倒是合法(经 `0x66` 操作数大小前缀),但内核里一律用 64 位的 `pushq %rax`,既保住 8 字节栈对齐、又统一风格,省得在不同位宽间出岔子。

> 外部依据:Intel SDM Vol.2B *PUSHA/PUSH—Push All General-Purpose Registers* 的 `#UD If in 64-bit mode` 条目;System V AMD64 ABI 的 *3.2.2 The Stack Frame* 节定义了 `RSP % 16 == 8` 的入口对齐约定。

---

### 参考

- Intel SDM Vol.2A — §3.1.1 内存操作数寻址(`disp(base,index,scale)` / SIB 编码)、*MOV—Move* 指令页(操作数类型与控制寄存器读写)。
- Intel SDM Vol.2B — *LGDT/SGDT* (加载/存储 GDTR)、*RDMSR/WRMSR* (`ECX` 选址、`EDX:EAX` 传 64 位值)、*IRET/IRETQ* (弹 `RIP/CS/RFLAGS/RSP/SS`)、*PUSHA/POPA* (`#UD in 64-bit mode`)。
- Intel SDM Vol.3A — §2.5 控制寄存器(CR0/CR3/CR4 各位语义)、Chapter 10 模式切换(CR0.PE / EFER.LME / CR0.PG 的开启顺序)。**这些是正文 001–003 讲语义的依据,本章只借其指令形态。**
- Intel SDM Vol.4 — *Model-Specific Registers*:MSR 地址清单(`MSR_GS_BASE=0xC0000101`、`MSR_KERNEL_GS_BASE=0xC0000102`、`MSR_EFER=0xC0000080`)。本地 PDF:`document/reference/intel/SDM-Vol4-Model-Specific-Registers.pdf`。
- OSDev — [Memory Addressing](https://wiki.osdev.org/Memory_Addressing)、[Inline Assembly](https://wiki.osdev.org/Inline_Assembly)(段前缀、`disp(base,index,scale)` 的社区版速查)。
- GCC 在线手册 — *18 Using Assembly Language with C / 18.1 AT&T Syntax*(立即数 `$`、寄存器 `%`、内存无前缀的官方定义)。
- System V AMD64 ABI — §3.2.2 栈帧与 `RSP % 16 == 8` 入口对齐。
- 本仓库源码:[context_switch.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/context_switch.S)、[long_mode.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/common/long_mode.S)、[stage2.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/stage2.S)、[serial.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/common/serial.S)、[mbr.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/mbr.S)、[interrupts.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/interrupts.S)。

> Intel SDM 版本说明:本仓库本地 PDF 为 2023-06 版,章节已重排。控制寄存器在 Vol.3A §2.5、MSR 在 Vol.4、指令参考在 Vol.2A/2B。引用以**章节/指令标题**为准,别拘泥于旧版编号。

> 参考 URL 的有效性会在全局审查阶段用 open-websearch(bing)统一核活,与本系列其它章节一致。
