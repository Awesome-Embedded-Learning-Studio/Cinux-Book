---
title: 03 · 代码路线:GDT、lgdt、CR0.PE、远跳、pm_entry
---

# 代码路线:GDT、lgdt、CR0.PE、远跳、pm_entry

源码主要在 [stage2.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/stage2.S)(VESA 之后的 PM 切换序列 + GDT 定义)和 [CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/CMakeLists.txt)(链接地址的改动)。

## 1. GDT:用一张扁平表取代段式寻址

GDT 定义在 [stage2.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/stage2.S) 末尾,单独放在 `.section .gdt` 里、`.align 8` 对齐:

```asm
.section .gdt,"a"
.align 8

gdt:
gdt_null:
    .quad 0                        # 第 0 项必须全 0(CPU 规定)

gdt_code:
    .word 0xFFFF                   # Limit 15:0
    .word 0x0000                   # Base 15:0
    .byte 0x00                     # Base 23:16
    .byte 0x9A                     # access: P|DPL=0|S=1|code|exec|read
    .byte 0xCF                     # flags(G=1,D=1)|Limit 19:16 = 0xF
    .byte 0x00                     # Base 31:24

gdt_data:
    .word 0xFFFF
    .word 0x0000
    .byte 0x00
    .byte 0x92                     # access: P|DPL=0|S=1|data|write
    .byte 0xCF
    .byte 0x00
```

把代码段的字节拼出来看:`access = 0x9A` = `1001 1010`——P(有效)=1、DPL(特权)=00、S=1(代码/数据段)、type=1010(代码、可执行、可读)。`flags = 0xC` = `1100`——G=1(4KB 粒度)、D=1(32 位默认操作数)。Limit 三段合起来是 `0xFFFFF`,配上 G=1 就是 `0xFFFFF × 0x1000 + 0xFFF = 4GB`。Base 三段全是 0——**段基址就是 0,段覆盖整个 4GB 空间**,这就是"扁平模型":段透明,地址即线性地址。数据段只把 access 换成 `0x92`(把"可执行"去掉、保留"可写"),其余一样。

> 这里有个**源码注释和实现不符**的地方,值得拎出来说:`gdt_code` 的 `.word 0x0000` 那行源码注释写着 "Base 15:0 (= 0x8000)",但实际编码出来的 base 是 **0**,不是 0x8000。这是对的——扁平模型必须 base=0,否则进 PM 后 `CS` 基址是 0x8000,而 `pm_entry` 又是按 0x8000 链接的绝对地址,两者一加就错位崩了。注释是笔误,代码是正确的。读这段源码时别被注释带偏。(这和 [002](../03-big-kernel/002/) 里 TSS 注释写成 "Table 8-2" 是同一类问题——源码注释是线索,不是权威。)

`gdt_ptr` 是给 `lgdt` 用的 6 字节结构(16 位 limit + 32 位 base):

```asm
gdt_ptr:
    .word (gdt_end - gdt - 1)      # limit = 表长 - 1
    .long gdt                      # base = GDT 的线性地址
```

`gdt_end - gdt - 1 = 23`(3 项 × 8 − 1),`gdt` 这个标号经链接后是它在 `0x8000` 之后的绝对地址。

## 2. lgdt 与"为什么实模式要先 DS=0"

切模式的序幕是这样开的:

```asm
cli
movw $0, %ax
movw %ax, %ds          # DS = 0
lgdt gdt_ptr           # 装载 GDTR
```

`lgdt gdt_ptr` 这条指令,CPU 是按**当时所处的模式**来算 `gdt_ptr` 这个操作数地址的。此刻我们还在实模式,实模式的寻址是 `DS << 4 + 偏移`。`gdt_ptr` 经过 [CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/CMakeLists.txt) 里的链接脚本(Stage2 现在链接在 `. = 0x8000`)得到的是一个 `0x81xx` 左右的绝对偏移;如果我们不把 `DS` 清零,`DS<<4` 会再叠一个段的偏移上去,`lgdt` 就从错误的内存读 GDTR,直接崩。

所以**必须先 `DS=0`**:这样实模式寻址退化成 `0<<4 + 偏移 = 偏移本身`,正好等于那个绝对地址 `0x81xx`,也就是 GDT 真正所在的地方。

顺带说一句链接地址的改动。001 时 Stage2 链接在 `. = 0x0`、运行时靠 `DS=0x800` 承载位置(相对模型);002 改回链接 `. = 0x8000`(绝对模型)。原因正是 `lgdt` 和 PM 后的绝对寻址需要**链接地址 = 载入地址**——一旦进 PM、base=0,所有标号都得是它们真实的线性地址,不能再靠段寄存器去补差。CMakeLists 里那句注释 "link address MUST match the load address" 就是这个意思。

> 外部依据:Intel SDM Vol.3A §3.4.4(LGDT/GDTR 结构)、§9.9.1(切换到 PM 前的 GDTR 装载)。`lgdt` 本身只搬运那 6 个字节,**不校验 GDT 内容合法性**——合法性要到后续真正用某个段选择子时才查,这点很容易踩(见"调试现场")。

## 3. CR0.PE:拨动那一个开关

```asm
movl %cr0, %eax
orb $0x1, %al          # 置 bit 0 = PE
movl %eax, %cr0        # 写回 CR0
```

`CR0` 的 bit 0 叫 **PE(Protection Enable)**。置 1 的这一刻,CPU "名义上"已经是保护模式了。但注意:**置位之后,CPU 仍然在用旧的 `CS`、旧的 16 位译码方式执行**——它不会自动刷新。这就埋下了下一节的那个关键动作。

全程 `cli` 不是可有可无。我们此刻**没有 IDT**(那是后面 big kernel 的事),一旦允许中断,任何异步中断(比如 PIT 定时器)进来找不到处理程序,直接三重故障重启。所以从 `cli` 到 `pm_entry` 之间,中断必须一直关着。

## 4. 远跳:不刷新 CS 就不算真正进入 PM

```asm
ljmp $0x08, $pm_entry

.code32
pm_entry:
    ...
```

这一句是整章的命门。`CR0.PE` 置了 1,但 CPU 还在用实模式遗留下来的 `CS` 和 16 位译码。要让保护模式"生效",必须强制 CPU 用**新的 GDT** 重新加载 `CS`。能干这件事的只有远跳/远调用一类指令——它们会带着一个新的段选择子(`0x08`),触发 CPU 去 GDT 查这个选择子、把 `CS` 换成对应的 32 位代码段,同时把译码切成 32 位。跳的目标 `pm_entry` 紧跟一个 `.code32`,告诉汇编器从这里开始按 32 位编码。

那如果置了 `CR0.PE` 却不 far jump,会怎样?CPU 会继续用 16 位译码执行后面的 32 位指令,译码错位,几条之内就执行到非法指令,三重故障重启。置 PE 和远跳必须成对出现,中间不能干别的要紧事。

这里有个 `.code16`/`.code32` 的认知点:`.code16` 和 `.code32` **是给汇编器看的指令,不是给 CPU 的**。它们决定汇编器把后面的指令编成 16 位还是 32 位机器码;真正决定 CPU 用哪种译码的是 `CS` 指向的段的 D 位(我们 GDT 里设的 D=1)。所以顺序必须对:远跳**之前**是 `.code16`(因为那时 CPU 还在 16 位译码,指令得编成 16 位才对得上),远跳**之后**的 `pm_entry` 才是 `.code32`。两者错位——比如把 `lgdt` 错放在 `.code32` 后面——CPU 实际还在 16 位译码,却拿到了 32 位编码的指令,又是错位崩溃。

> 外部依据:Intel SDM Vol.3A §9.9.2 明确:进入 PM 后的第一件事必须是远跳(或等价的远调用)来加载一个新的代码段选择子,以"冲掉"实模式遗留的 `CS`。

## 5. pm_entry:新的段、新的栈,还有 0xE9 debugcon

进了 `pm_entry`,我们已经站在 32 位保护模式里。`CS` 已经被远跳设好了,但 `DS/ES/FS/GS/SS` 还带着实模式留下的脏值,得手动刷成数据段选择子 `0x10`,再换个新栈:

```asm
.code32
pm_entry:
    movw $0x10, %ax          # 数据段选择子
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    movl $0x90000, %esp      # PM 下的新栈(0x90000,实模式旧栈 0x9000/0xFFFE 不再适用)

    movb $0x50, %al          # 'P'
    outb %al, $0xE9          # 写 debugcon

    cli
.pm_halt:
    hlt
    jmp .pm_halt
```

栈为什么从 `0x9000:0xFFFE` 换成 `0x90000`?因为实模式栈地址是 `SS<<4 + SP`(16 位段),进了 PM 扁平模型,栈地址就是 `ESP` 一个 32 位数;旧的 `0x9000:0xFFFE` 在新模型下会被当成 `ESP=0xFFFE`,那是 64KB 附近、非常低且危险的地方。换到 `0x90000`(576KB)给它一个安稳的家。

`outb %al, $0xE9` 是这一章新引入的输出手段。QEMU 的 **debugcon** 设备挂在端口 `0xE9`,往它写一个字节,QEMU 就把字节记到一个文件里([qemu.cmake](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/cmake/qemu.cmake) 里配了 `-debugcon file:debug.log -global isa-debugcon.iobase=0xe9`)。为什么需要它?因为进 PM 后 `INT 0x10` 没了(告别 BIOS),屏幕又是 VESA 图形模式(没字体、不能 teletype),我们陷入了"既没 BIOS、又没屏幕、又没串口"的输出真空。debugcon 是这个真空期里最便宜的可观测手段——写一个 `P` 到 `build/debug.log`,就知道 `pm_entry` 真的执行到了。注意它**不是真串口**(串口是 COM1/端口 `0x3F8`,驱动要等 [005](../03-big-kernel/005/)),只是个 QEMU 专用的调试后门。
