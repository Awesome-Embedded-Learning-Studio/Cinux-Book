---
title: 01 · GAS 语法骨架
---

# 01 · GAS 语法骨架:为什么 Cinux 全程用 AT&T

> 这一章把 GAS/AT&T 立为唯一语法本位,讲清"读得懂正文 001/003"所需的最小骨架——四条铁律、指令后缀、段与伪指令、数据定义、宏与数字标号。读完你不会变成汇编高手,但能在正文里任何一段 `.S` 看到怪写法时,知道它在做什么、为什么这么写。

## 为什么是 GAS/AT&T

Cinux 的所有 `.S` 文件(`boot/mbr.S`、`boot/common/long_mode.S`、`kernel/arch/x86_64/interrupts.S`……)用的都是 GNU 汇编器 **GAS**(GNU Assembler,即 `as`)的 **AT&T 语法**。这与很多中文汇编教材默认的 **NASM/Intel** 写法长得不一样,先记住**四条铁律**,后面所有代码都围着它转:

1. **源在左、目的在右**——`mov` 是 `mov 源, 目的`。Intel 写 `mov ax, cs`(把 `cs` 给 `ax`),AT&T 反过来:
   ```asm
   movw %cs, %ax      // %cs(源) → %ax(目的)
   ```
   这条直接来自 [mbr.S:58-60](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/mbr.S) 里理顺段寄存器的那几行——把 `cs` 抄到 `ax`,再分发到 `ds/es/ss`。

2. **寄存器加 `%`、立即数加 `$`**——`%ax` 是寄存器,`$0x7000` 是立即数。少一个符号,GAS 就把你当成标号或数字去解析,报错位置还特别离谱。看设栈那一行:
   ```asm
   movw $0x7000, %sp   // $0x7000 → %sp:把立即数 0x7000 放进 sp
   ```
   注意 `0x7000` 前面那个 `$` 不能省——省了就成了"把地址 0x7000 处的内容"搬过来,意思完全不同。

3. **内存操作数无前缀,用圆括号**——Intel 的 `[bx]` 在 AT&T 里是 `(%bx)`,`[bx+si+4]` 是 `4(%bx,%si)`。偏移写在最前面,括号里是基址/变址/比例。`mbr.S:119` 那一串写 DAP 字段就是这个样子:
   ```asm
   movb $DAP_SIZE, (%si)        // $0x10 → (%si):写到 si 指向的字节
   movw $STAGE2_SECTORS, 2(%si) // → (%si)+2:写到偏移 2 处的一个字
   movl $STAGE2_LBA, 8(%si)     // → (%si)+8:写到偏移 8 处的双字
   ```
   后缀 `b/w/l` 后面讲;这里先盯住圆括号——内存访问一律是 `disp(base, index, scale)`,没有方括号、没有 `byte ptr`。

4. **注释用 `//`(或 `#`),不是 `;`**——GAS 把 `//` 和 `#` 当行注释。Cinux 全程用 `//`,行尾补一句"为什么"。`;` 在 GAS 里是语句分隔符,不是注释。

> 为什么 Cinux 选 GAS 而不是更"易读"的 NASM?因为 GAS 是 GCC 工具链的原配:内核里那些 `.S` 要和 C 代码一起进同一套 `gcc -c` 编译、靠 `.global`/`.extern` 和 C 函数互相调用(interrupts.S 里 `call \handler` 调的就是 C++ 写的中断处理函数),GCC 内联汇编(`asm volatile(...)`)也只产 AT&T。换 NASM 就得在两套语法、两个汇编器之间来回翻译,得不偿失。一句话:**跟着工具链走,省掉一整层心智负担**。

## 指令后缀 b/w/l/q:裸机多模式不能省

AT&T 的 `mov`/`add`/`push` 这类指令通常要带一个**操作数宽度后缀**:

```text
b = byte   (8 位,  1 字节)
w = word   (16 位, 2 字节)
l = long   (32 位, 4 字节)
q = quad   (64 位, 8 字节)
```

所以同样是"搬一个值",Cinux 写的是 `movb`、`movw`、`movl`、`movq`,四种各有各的宽度:

```asm
movb %dl, boot_drive        // 字节:dl → 内存(mbr.S:72)
movw $0x4200, %ax           // 字:  立即数 → ax(mbr.S:129,实模式 16 位)
movl $STAGE2_LBA, 8(%si)    // 双字:立即数 → 内存(mbr.S:124)
pushq %rax                  // 四字:寄存器入栈(interrupts.S:39,长模式 64 位)
```

为什么不能像 Intel 那样写个光秃秃的 `mov` 让汇编器去猜?**因为在实模式(16 位)、保护模式(32 位)、长模式(64 位)三种 CPU 模式之间反复横跳时,"猜"会出人命——尤其对内存操作数。** 操作数里只要带寄存器(如 `mov $0x7000, %sp`),宽度就被寄存器钉死(`%sp` 是 16 位,恒为 `movw`),汇编器其实不用猜;真正危险的是**只有立即数 + 内存、没有寄存器**的写法,比如 `mov $0x10, (%si)`——这种情况下汇编器无从推断宽度,只能按当前 `.codeXX` 的默认位宽编:在 `.code16` 下当 word、在 `.code32` 下当 long。要是在模式切换的边界上漏写了后缀,生成的机器码宽度就可能和你想的不一样,黑屏重启都查不出原因。

Cinux 的解法是**显式写死宽度**——后缀写明白,不依赖"当前段的默认位宽"。同一份 `boot/` 代码里,`.code16` 段用 `movw`(见 [mbr.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/mbr.S)),`.code32` 段用 `movl`(见 [long_mode.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/common/long_mode.S) 的 `setup_page_tables`),`.code64`/内核段用 `pushq`/`popq`(见 interrupts.S)。模式由 `.code16/.code32/.code64` 伪指令声明,宽度由后缀钉死,两条线分开管。

## 段与伪指令:把代码"分装"进不同抽屉

GAS 用**伪指令**(以 `.` 开头、不是真机器指令的汇编器指令)来告诉汇编器"这段东西放哪个段、按什么模式编、要不要导出给链接器"。Cinux 用到的就这么几条:

```asm
.section .text          // 进 .text 段(代码)
.code16                 // 以下按 16 位实模式编码(mbr.S:30)
.code32                 // 以下按 32 位保护模式编码(long_mode.S:73)
.code64                 // 以下按 64 位长模式编码
.global _start          // 把 _start 导出,链接器才找得到入口(mbr.S:31)
.extern print_string_mbr // 声明:这个符号在别的文件里定义(mbr.S:33)
.set STAGE2_LBA, 1      // 定义编译期常量 STAGE2_LBA = 1(mbr.S:20)
```

`.code16/.code32/.code64` 是 Cinux 裸机多模式的命脉。同一份 `boot/` 要先在实模式读盘(mbr.S 用 `.code16`),再切到保护模式搭页表(long_mode.S 用 `.code32`),最后跳进长模式(`.code64`)。这三个伪指令相当于对汇编器说:"从这里开始,按这个位宽翻译接下来的指令。" 模式切换本身靠 `ljmp` 远跳和改 `CR0`/`EFER` 完成,但**编码位宽**是 `.codeXX` 管的——两件事不要混。

> 注意 `.code16` 不是"切换 CPU 模式",它只影响**汇编器生成的机器码宽度**。CPU 真正进哪种模式,是运行时靠 `CR0.PE`、`EFER.LME` 这些控制位决定的。这是初学裸机最容易踩的认知坑——"我写了 `.code32` 怎么 CPU 还在实模式?" 因为 `.code32` 编的是码,不是状态。

`.global`/`.extern` 管**符号的可见性**:比如 `long_mode.S` 里 `.global setup_page_tables`(long_mode.S:87)把符号导出给链接器,`stage2.S` 那边用 `.extern setup_page_tables`(stage2.S:44)声明、`call setup_page_tables`(stage2.S:178)调用——这是两个汇编文件之间的跨文件调用。C++ 调汇编同理:`kernel/main.cpp:81` 的 `extern "C" void irq_init();` 声明后,就能调到汇编里 `.global` 导出的同名函数。反过来,`interrupts.S` 里 `call \handler` 调的 C++ 中断处理函数,GAS 允许不写 `.extern`、直接 `call`,符号由链接器解析到 C++ 那边 `extern "C"` 导出的实现。`.set` 则是编译期常量,`mbr.S` 开头那一坨 `.set STAGE2_LBA, 1` / `.set PAGE_FLAGS, (...)`(long_mode.S:37)就是给魔法数字起名字,改起来只动一处。

这些伪指令的**实模式/保护模式/长模式实战细节**,正文展开得很透——详见正文 [001 · 实模式引导](../../book/01-boot/001-boot-real-mode.md)(`mbr.S`/`stage2.S` 的实模式用法)、[002 · GDT 与保护模式](../../book/01-boot/002-boot-gdt-protected.md)、[003 · 进入长模式](../../book/01-boot/003-boot-long-mode.md)(`long_mode.S` 的 `.code32`→`.code64` 切换)。本前置卷只摆骨架,不重复讲。

## 数据定义:把字节铺进镜像

光有代码不够,引导阶段还要往镜像里塞字符串、魔数、字体。GAS 的数据定义伪指令:

```asm
.byte 0              // 1 字节(mbr.S:154,boot_drive 占位)
.word 0xAA55         // 2 字节(mbr.S:158,MBR 魔数)
.long font_psf_end - font_psf_start   // 4 字节(font_data.S:30,字体大小)
.quad 0              // 8 字节(stage2.S:316,GDT 空描述符)
.asciz "Cinux Booting...\r\n"         // 带结尾 \0 的字符串(mbr.S:148)
```

`.byte/.word/.long/.quad` 分别填 1/2/4/8 字节,`.asciz` 填字符串**并自动补 `\0`**(配合正文里 `lodsb` 读到 0 就停的 `print_string`)。两条**布局伪指令**也常见:

```asm
.org 510             // 把"当前位置指针"强行拨到偏移 510(mbr.S:157)
.align 8             // 对齐到 2^3=8 字节边界(stage2.S:312)
```

`.org 510` 是 MBR 的命根子——BIOS 要求 512 字节扇区的**第 510、511 字节**必须是魔数 `0x55 0xAA`,所以代码再长也得让出最后两字节,用 `.org 510` 把指针顶到 510,再 `.word 0xAA55` 写魔数。`.align 8` 则保证后面的 GDT/页表满足对齐要求(页表要 4K 对齐靠地址常量保证,GDT 描述符靠 `.align`)。

还有一条特别实用的:`.incbin`,它**原样嵌进一个二进制文件**,不做任何解析。Cinux 内核的字体就是这么进来的:

```asm
.section .rodata
.global font_psf_start
font_psf_start:
    .incbin "assets/font.psf"   // 把字体文件字节流直接铺进来(font_data.S:22)
.global font_psf_end
font_psf_end:
```

`font_psf_start` 到 `font_psf_end` 之间就是字体文件的全部字节,`font_psf_size = font_psf_end - font_psf_start` 是它的长度。这一招比手写 `.byte` 列几千行聪明得多——把现成的 PSF 字体当资源嵌进内核 ELF,链接后符号干干净净。

## 宏与数字标号:ISR 不写二十遍

裸机里大量重复结构的活——比如"为每一种 CPU 异常写一个中断处理桩"。Cinux 有 20 多个异常,每个都要 push 一遍全部寄存器、调 C handler、再 pop 一遍。手写二十份?不现实。GAS 的 `.macro` 就是干这个的。

看 [interrupts.S:31-97](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/interrupts.S) 里的 ISR 宏(节选):

```asm
.macro ISR_NOERRCODE name handler       // 定义宏,两个参数:name 和 handler
.global \name                           // \name:引用参数 name(不是 NASM 的 %1!)
.type \name, @function
\name:
    pushq $0                            // 补一个假错误码,让栈布局统一
    pushq %rax                          // 存所有通用寄存器
    /* ...pushq 其余寄存器... */
    leaq 8(%rsp), %rdi                  // InterruptFrame* 当第一个参数
    call \handler                       // 调对应的 C 处理函数(第二个参数)
    /* ...popq 还原寄存器... */
    iretq
.endm
```

**两个关键点,和 NASM 完全不同**:

1. **`.macro` 用命名参数,引用时加反斜杠**——`\name`、`\handler`,**不是** NASM 的 `%1`/`%2`。这一点是 NoteBookProject 里那些 NASM 笔记照搬过来最容易翻车的:NASM 写 `%1`,GAS 写 `\name`。宏名后面直接列参数名,定义处 `name handler`,用的时候 `\name`/`\handler`。
2. **数字标号 `1:`/`2:`/`3:` 可以重复定义,用 `1b`/`1f` 回引**——`b` = backward(往后/向上找最近的 `1:`),`f` = forward(往前/向下找最近的 `1:`)。这样循环和局部跳转不用每次挖空心思想标号名。看 [long_mode.S:124-133](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/common/long_mode.S) 里填 2MB 页的那个循环:

```asm
    movl $4, %ecx          // 循环 4 次:PD[0..3]
1:                          // 数字标号(局部,可重复)
    movl %eax, %edx
    shll $21, %edx         // eax << 21:页基址(2MB 对齐)
    orl $PAGE_FLAGS, %edx
    movl %edx, (%edi)      // 写进 PD 表项
    addl $8, %edi          // 下一项(每项 8 字节)
    incl %eax
    loop 1b                // 1b = 往回找最近的 1:,即本循环开头
```

`1:` 是局部数字标号,在这个文件里别的函数也能再定义一个 `1:` 而不冲突,`loop 1b` 永远指向**当前代码往前最近**的那个 `1:`。比起 NASM 用 `.loop:` 这种点前缀的局部标号,GAS 的数字标号更适合"一段循环里只有一个回跳点"的朴素写法。

`.macro` + 数字标号这套组合拳,让 interrupts.S 能用一行 `ISR_NOERRCODE isr_de_stub, handle_de` 就展开出一整个合规的中断桩,二十几个异常写起来跟填表一样。这是 Cinux 选 GAS 的另一个现实收益——**和 GCC 内联汇编、C/C++ 符号互通**,宏展开、`.global` 导出、`.extern` 调用,全在一套工具链里。

---

### 参考

- GCC 手册 — [Using Assembly Language with GCC](https://gcc.gnu.org/onlinedocs/gcc/Using-Assembly-Language-with-GCC.html)(AT&T 语法、`%`/`$` 前缀、寄存器/内存操作数约定)、[Assembler Options](https://gcc.gnu.org/onlinedocs/gcc/Assembler-Options.html)(`.code16/.code32/.code64` 与 `-Wa,--32`)。
- GAS 手册 — [Pseudo Ops](https://sourceware.org/binutils/docs/as/Pseudo-Ops.html)(`.set/.global/.extern/.org/.align/.incbin/.asciz`)、[Macros](https://sourceware.org/binutils/docs/as/Macro.html)(`.macro/.endm`、`\name` 命名参数)、[Labels](https://sourceware.org/binutils/docs/as/Labels.html)(数字标号 `1:` 与 `1b`/`1f` 回引)。
- OSDev — [AT&T Syntax](https://wiki.osdev.org/AT%26T_Syntax)(与 Intel/NASM 对照)、[Inline Assembly](https://wiki.osdev.org/Inline_Assembly)(为什么 GCC 生态统一 AT&T)。
- 本仓库源码:[mbr.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/mbr.S)(`.code16`、`.set`、`.asciz`、`.org`、`movb/movw/movl`)、[long_mode.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/common/long_mode.S)(`.code32`、数字标号 `1:`/`1b`)、[interrupts.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/interrupts.S)(`.macro`/`\name`/`\handler`、`pushq`)、[font_data.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/video/font_data.S)(`.incbin`、`.long`)、[stage2.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/stage2.S)(`.align`、`.quad`、`.word`)。

> 参考 URL 的有效性会在全局审查阶段用 open-websearch(bing)统一核活,与本系列其它章节一致。
