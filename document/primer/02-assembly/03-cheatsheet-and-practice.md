---
title: 03 · AT&T↔Intel 速查表与 GAS 实战
---

# 03 · AT&T↔Intel 速查表与 GAS 实战:从一张表到点亮第一段代码

> 这一章把模块 2 收口:给一张"看到 Intel 写法能立刻翻成 AT&T"的全要素对照表,再把 `gcc -c` / `ld -T` / `objcopy` 这条最小编译回路走通,最后用它逐行读懂 Cinux 的第一段代码 `mbr.S`。读完你应能独立打开任何一个 `.S` 文件不卡壳。

## 这一章干什么

前两章([01 语法骨架](01-gas-syntax-skeleton.md)、[02 寻址与系统指令](02-addressing-system-instr.md))把 GAS/AT&T 的"为什么"和"怎么写"拆开讲透了。现实里我们不会凭空发明写法——会不停地在两套语法之间来回对照:**网上绝大多数 x86 教程、NoteBookProject 里那些汇编笔记,都是 Intel/NASM 写的**。我们要做的是拿一张速查表当翻译字典,把看到的 Intel 一行翻成 AT&T,再喂给 GAS。

所以这章分三段,每段都只用"够读懂正文 001/004 的最小集",细节能跳正文的就跳:

1. **全要素对照表**——寄存器、立即数、内存、寻址、伪指令、宏,每行配 Cinux 真实例子。重点啃透那张 GDT 描述符 `.quad 0x00AF9A000000FFFF` 是怎么从一个 access byte `0x9A` + flags `0xAF` 拼出来的。
2. **最小编译回路**——照着 `boot/CMakeLists.txt` 的真实写法,把一条 `.S` 变成可链接的 `.o`、再变成裸 `.bin`。顺带把两个最常见的报错(`suffix disagreement` / `relocation truncated`)的根因讲清楚。
3. **读 `mbr.S` 收官**——用对照表逐行解释前半段。`mbr.S` 里 BIOS 怎么跳进来、段怎么理顺、怎么读盘,细节正文 [001 · 实模式引导](../../book/01-boot/001-boot-real-mode.md) 已讲透,这里只做"对照表实战"的演示。

## 一、AT&T ↔ Intel 全要素对照表

先把规则钉死,再贴表。**AT&T 四条铁律**(第 1 章已展开,这里复习):

- 寄存器加 `%`、立即数加 `$`、内存操作数**无前缀**;
- **源在左、目的在右**(Intel 是目在左);
- 指令后缀 `b/w/l/q` 标尺寸,当两个操作数里没有寄存器时**必须**靠它告诉汇编器宽度;
- 注释用 `//` 或 `#`(GAS 里 `#` 是单行注释,Intel/NASM 里 `;` 才是)。

> 外部依据:GCC 手册《Using the GNU Compiler Collection》的 *Options for Code Generation Conventions* 一节,以及 GAS 文档《Using as》的 *i386-Syntax* / *i386-Memory* 章节,逐条定义了 AT&T 的 `%`/`$` 前缀、源目顺序、`disp(base,index,scale)` 寻址语法与 `b/w/l/q` 后缀规则。OSDev 的 [X86 Assembly](https://wiki.osdev.org/X86_Assembly) 与 [AT&T Syntax](https://wiki.osdev.org/AT%26T_Syntax) 页有社区整理的中英对照。

下面每行右侧(Intel 列)取自 NoteBookProject 汇编笔记的真实写法,左侧是我们翻译后 Cinux 实际在用的 AT&T。

| 要素 | Intel/NASM | AT&T(GAS) | Cinux 实例 |
|------|-----------|-----------|-----------|
| 寄存器→寄存器 | `mov ax, cs` | `movw %cs, %ax` | [mbr.S:58](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/mbr.S) |
| 立即数→寄存器 | `mov ax, 0x7000` | `movw $0x7000, %sp` | [mbr.S:67](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/mbr.S) |
| 寄存器→内存(直接偏移) | `mov [si], 0x10` | `movb $0x10, (%si)` | [mbr.S:119](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/mbr.S)(`$DAP_SIZE` 宏展开为 `0x10`) |
| 带位移的内存 | `mov [si+2], cx` | `movw %cx, 2(%si)` | [mbr.S:120](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/mbr.S) |
| 远转移 | `jmp 0:real_start` | `ljmp $0, $real_start` | [mbr.S:47](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/mbr.S) |
| 段前缀 | `mov word [es:di], 0x4256` | `movw $0x4256, %es:(%di)` | [serial.S:140](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/common/serial.S) 写 VBE2 签名 |
| 自身异或清零 | `xor ax, ax` | `xorw %ax, %ax` | [mbr.S:57](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/mbr.S) |
| 数据定义(字节) | `db 0` | `.byte 0` | [mbr.S:154](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/mbr.S) |
| 数据定义(字) | `dw 0xAA55` | `.word 0xAA55` | [mbr.S:158](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/mbr.S) |
| 数据定义(字符串) | `db "Hi",0` | `.asciz "Hi"` | [mbr.S:148](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/mbr.S) |
| 常量等价 | `STAGE2_LBA equ 1` | `.set STAGE2_LBA, 1` | [mbr.S:20](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/mbr.S) |
| 当前地址 / 对齐 | `times 510-$+$$ db 0` | `.org 510`(到指定偏移) | [mbr.S:157](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/mbr.S) |
| 宏参数 | `%1`/`%2`(位置) | `\1`/`\2`(位置)或 `\name`(命名) | interrupts.S 的 `.macro`(用命名) |

几个**最容易翻车**的点,单独拎出来:

- **`movw %cs, %ax` 不是 `movw %ax, %cs`**。AT&T 源在左,段寄存器 `cs` 是源、`ax` 是目的。习惯了 Intel 的 `mov ax, cs`(目在左)最容易在这里写反。
- **`2(%si)` 不是 `[si+2]`**。AT&T 的内存寻址是 `disp(base, index, scale)`,位移在最外面、括号里才是基址/变址寄存器。第 2 章讲过完整形式,这里只用得到最简的 `disp(base)`。
- **`(%si)` 括号不能省**。写成 `%si` 是寄存器本身(把 si 的值传走),写成 `(%si)` 才是"si 指向的内存"。BIOS 读盘那几行 `movb $0x10, (%si)` 把立即数写进 si 指向的内存,少了括号就变成"写进 si 寄存器",语义全错。
- **没有寄存器参与时,后缀不能省**。`movb $0x10, (%si)` 里如果写成 `mov $0x10, (%si)`,汇编器无法从两个操作数推断宽度(立即数没尺寸、内存也没尺寸),直接报 `suffix disagreement`——这正是下一节要讲的头号报错。

### 啃透那张 GDT 描述符:`.quad 0x00AF9A000000FFFF`

对照表里有一行特别值得展开,因为它把"位运算 + 字段布局 + 两种语法"全揉在了一个 64 位数里——这正是正文 [002 · 进入保护模式](../../book/01-boot/002-boot-gdt-protected.md) 要建 GDT 时绕不开的东西。

在 [stage2.S:339-340](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/stage2.S) 有这么一行:

```asm
// 64-bit code descriptor (L=1, D=0)
// Value: 0x00AF9A000000FFFF
gdt_code64:
    .quad 0x00AF9A000000FFFF       // 64-bit code descriptor (L=1, D=0)
```

这一个 `.quad`(64 位整数)其实是一条**手工拼好的段描述符**。Intel 的段描述符是 8 字节,但它的字段在内存里**不是顺序排的**——base 和 limit 都被拆成了几截,塞在不连续的位上。把 `0x00AF9A000000FFFF` 从高字节到低字节拆开看:

```text
字节   值      字段(按 Intel SDM Vol.3A §3.4.5 / Figure 3-8 描述符布局)
7      0x00    Base 31:24
6      0xAF    Flags(G/L/D_B/AVL)+ Limit 19:16   ← 高 4 位是 flags
5      0x9A    Access byte(P/S/DPL/Type)          ← 访问字节
4      0x00    Base 23:16
3..2   0x0000  Base 15:0
1..0   0xFFFF  Limit 15:0
```

两个关键字节拆到**位**:

**Access byte = `0x9A` = `1001 1010`**:

```text
位7  P    =1  Present        段在内存里
位6-5 DPL =00 特权级 0        内核态用
位4  S    =1  代码/数据段     (0 才是系统段/TSS)
位3-0 Type=1010 代码段: Execute + Read (可执行可读)
```

**Flags 字节 = `0xAF` = `1010 1111`**:

```text
位7 G     =1  粒度 4KB        limit 要乘 4096
位6 D/B   =0  (见下)
位5 L     =1  64 位代码段     ← 这一位决定了它是 long-mode 描述符
位4 AVL   =0  软件可用位
位3-0 Limit19:16 = 1111       limit 高 4 位
```

把 limit 的两截 `0xFFFF`(低 16 位)和 `0xF`(高 4 位)拼起来是 `0xFFFFF`,因为 `G=1` 按 4KB 粒度,最终段限 = `0xFFFFF << 12 | 0xFFF = 0xFFFFFFFF`,也就是整整 4GB——全覆盖,不做越界检查。`L=1` 这一位是 64 位段描述符的标志(Intel SDM 称为 *L bit: 64-bit code segment*),它和 `D/B=0` 是配套的:**`L=1` 时 `D/B` 必须为 0**,否则触发 `#GP`。

对比一下同一段里的 32 位代码段 [stage2.S:318-324](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/stage2.S),它没有用 `.quad` 一把写死,而是用五个 `.word/.byte` 拆开摆:

```asm
gdt_code:
    .word 0xFFFF                   // Limit 15:0
    .word 0x0000                   // Base 15:0
    .byte 0x00                     // Base 23:16
    .byte 0x9A                     // Access: P=1 DPL=0 S=1 Type=code exec/read
    .byte 0xCF                     // Flags: G=1 D/B=1 ...  ← 这里 D/B=1 表示 32 位段
    .byte 0x00                     // Base 31:24
```

注意这里 flags 是 `0xCF` 而不是 `0xAF`——区别就在**第 6 位 `D/B` 和第 5 位 `L`**:`0xCF = 1100 1111`,D/B=1(32 位段)、L=0;`0xAF = 1010 1111`,D/B=0、L=1(64 位段)。两种写法(一把 `.quad` vs 拆 `.byte`)在内存里**最终字节完全一样**,`gdt_code64` 用 `.quad` 只是因为 64 位段常用、且 base/limit 都是规整的全 0/全 F,一把写更省事。

> 外部依据:Intel SDM Vol.3A §3.4.5「Segment Descriptors」(本仓库本地 PDF `document/reference/intel/SDM-Vol3A-...Part1.pdf` 第 3-9 ~ 3-11 页,Figure 3-8 描述符布局图)逐字段定义了 Base/Limit/Type/S/DPL/P/G/D-B/L 各位的含义与编码;§3.4.5 里明确写出 "L — 64-bit code segment (IA-32e mode only)" 且 `L=1` 时 `D/B` 必须为 0。OSDev 的 [GDT Tutorial](https://wiki.osdev.org/GDT_Tutorial) 有社区整理的 access byte / flags 速查表。

这些字段的**运行时含义**(P/DPL/Type 怎么被 CPU 检查、`lgdt` 怎么加载、为什么 64 位模式下多数段寄存器被忽略)详见正文 [002](../../book/01-boot/002-boot-gdt-protected.md);这里只管"这张表怎么手算出来"。

## 二、最小编译回路:从 `.S` 到 `.bin`

GAS 写的 `.S`(大写 S 会先过 C 预处理器,`.s` 小写不会——Cinux 全用大写,因为要 `#include`/`.set` 之类)变成可引导的裸二进制,标准三步:**汇编 → 链接 → 抽裸**。这三步 Cinux 全写在 [boot/CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/CMakeLists.txt) 里,我们照着拆。

### 1. 汇编:`gcc -c`(经 `-Wa,--32` 生成 32 位目标)

Cinux 没有手敲 `as`,而是走 `gcc` 当驱动(CMake 里用 `target_compile_options`):

```cmake
add_executable(mbr mbr.S)
target_compile_options(mbr PRIVATE
    -Wa,--32                    # 交给汇编器:生成 32 位 ELF 目标
)
```

等价的命令行是:

```bash
gcc -c -Wa,--32 mbr.S -o mbr.o      # mbr.o 是 elf32-i386 目标文件
```

这里有个**只有 bootloader 才有的别扭**:`mbr.S` 顶上是 `.code16`(见 [mbr.S:30](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/mbr.S)),生成的指令是 16 位的;但 `-Wa,--32` 让目标文件是 **32 位 ELF**。这是故意的——16 位代码可以装在 32 位 ELF 里(靠 `.code16` 逐指令加 `0x66/0x67` 前缀),链接器只认 32 位 ELF 才能正常做重定位。注释 `# Assemble 16-bit code as 32-bit objects` 说清了这套配合。

### 2. 链接:`ld -T`(链接脚本钉住加载地址)

```cmake
target_link_options(mbr PRIVATE
    -Wl,-m,elf_i386
    -T ${CMAKE_CURRENT_BINARY_DIR}/mbr.ld
    -nostdlib
    -no-pie
)
```

关键是 `-T mbr.ld`——链接脚本。它干两件事:**定加载地址**、**决定段顺序**。MBR 的链接脚本是 CMake 在构建时 `file(WRITE)` 生成的([CMakeLists.txt:83-98](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/CMakeLists.txt)):

```cmake
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/mbr.ld "
OUTPUT_FORMAT(\"elf32-i386\")
ENTRY(_start)
SECTIONS
{
    . = 0x7C00;              ← 把起点钉在 BIOS 约定的 0x7C00
    .text : { *(.text) *(.rodata) }
    .data : { *(.data) }
    .bss  : { *(.bss) }
    /DISCARD/ : { *(.comment*) *(.note*) }
}
")
```

`. = 0x7C00` 是整章的灵魂:它告诉链接器"假装我的代码会被放在物理地址 `0x7C00`"。于是 `mbr.S` 里所有标号(比如 `msg_booting`、`real_start`)算出来的地址都是 `0x7C00` 附近的值——正好和 BIOS 把 MBR 读进 `0x7C00` 的事实对上。地址对不上,实模式下取标号就会取到垃圾。`/DISCARD/` 把编译器塞进来的 `.comment`/`.note` 段扔掉,省字节(MBR 只有 512 字节预算,一个字节都不能浪费)。

`-nostdlib` 是必须的:我们不要 libc、不要 `_start` 之外的启动文件,MBR 自带 `_start`。`-no-pie` 关掉位置无关可执行文件——bootloader 要的是"地址写死",PIE 会把地址延迟到加载时重定位,跟"钉死 0x7C00"的初衷冲突。

等价命令行:

```bash
ld -m elf_i386 -T mbr.ld -nostdlib -no-pie mbr.o -o mbr.elf
```

### 3. 抽裸:`objcopy -O binary`

链接出来的是 ELF(`mbr.elf`),带一堆头和段表,几百字节的"毛刺"。BIOS 只认**裸二进制**——它会把第 0 扇区那 512 字节原封不动读进内存执行,根本不懂 ELF 头。所以最后一步是抽裸:

```cmake
add_custom_command(
    TARGET mbr POST_BUILD
    COMMAND objcopy -O binary $<TARGET_FILE:mbr> $<TARGET_FILE_DIR:mbr>/mbr.bin
    COMMENT "Converting MBR to raw binary: mbr.bin"
)
```

```bash
objcopy -O binary mbr.elf mbr.bin      # mbr.bin 就是纯指令字节,512 字节
```

抽完裸,`mbr.S` 末尾的 `.org 510` + `.word 0xAA55`([mbr.S:157-158](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/mbr.S))就把第 510、511 字节填成了魔数 `0xAA55`——BIOS 靠这两个字节判断"这是不是一个合法的引导扇区"。这一步产物必须是 512 字节,正文 001 的验证步骤就是卡这个。

整条回路画出来:

```text
mbr.S ──gcc -c -Wa,--32──▶ mbr.o(elf32 目标)
        ──ld -T mbr.ld──▶ mbr.elf(带 ELF 头,地址钉 0x7C00)
        ──objcopy -O binary──▶ mbr.bin(512 字节裸二进制,末尾 0xAA55)
```

### 4. 两个最常见的报错根因

实模式汇编最容易撞这两个错,根因都和"宽度对不上"或"地址塞不下"有关。

**报错一**:`Error: suffix or operand size mismatch` / `suffix disagreement`。

意思是汇编器**推断不出这条指令的操作数宽度**。典型触发:

```asm
mov $0x10, (%si)      // ✗ 立即数没尺寸、内存没尺寸,汇编器不知道是写 1/2/4 字节
movb $0x10, (%si)     // ✓ 加后缀 b,明确写一字节
```

对照表里那条 `movb $0x10, (%si)`([mbr.S:119](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/mbr.S))就是靠 `b` 后缀告诉汇编器"写一字节"。规则很死:**只要两个操作数都不是寄存器**(立即数 → 内存、或内存 → 内存),就必须给后缀**或**用尺寸前缀(`movw`/`movl`)。习惯 Intel 的人特别容易漏——Intel 里 `mov byte ptr [si], 10h` 的尺寸是写在操作数上的,AT&T 是写在指令助记符上的。

**报错二**:`relocation truncated to fit: R_386_16 against ...` 或 `relocation truncated to fit: R_386_PC16`。

意思是某个 16 位重定位项**塞不下**链接后算出来的地址。典型触发:链接脚本把代码钉在 `0x8000` 这种高地址,但某条 16 位指令(如实模式下的 `ljmp`/`lcall` 或 16 位数据引用)的目标地址超过 16 位能表示的范围。Cinux 的对策在 [stage2.S:358-364](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/stage2.S) 那段注释里说得很直白:64 位 GDT 指针故意用 `.long` + `.long` 拆开写,**不用 `.quad`**,就是为了**避免在 32 位 ELF 里出现 64 位重定位**:

```asm
// Note: Use .long + .long instead of .quad to avoid 64-bit relocation
// in 32-bit ELF. GDT is at low address so upper 32 bits are zero.
gdt64_ptr:
    .word (gdt_end - gdt - 1)
    .long gdt              // 低 32 位
    .long 0                // 高 32 位(GDT 在低地址,这里就是 0)
```

如果这里写成 `.quad gdt`,链接器在 32 位 ELF(`elf_i386`)里没有 64 位重定位类型可用,就会报 `relocation truncated`。拆成两个 `.long` 就绕开了——GDT 反正在低地址,高 32 位恒为 0,写成常量 0 没问题。这是个**只有手写汇编才会踩**的坑,正文 [002](../../book/01-boot/002-boot-gdt-protected.md) 切到长模式时还会再提。

## 三、收官:用对照表读 `mbr.S` 前半段

把前两节的工具用起来,逐行读 [mbr.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/mbr.S) 从入口到读盘。细节(为什么 CS 要归零、DAP 各字段、为什么栈放 `0x7000`)正文 [001](../../book/01-boot/001-boot-real-mode.md) 已讲透,**这里只用对照表做翻译演示**,不重复。

开头一堆 `.set`([mbr.S:20-27](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/mbr.S))是 NASM `equ` 的 AT&T 版——给常量起名字,后续 `movw $STAGE2_LBA, 8(%si)` 里就能用符号而不是裸数字。`.section .text` / `.code16` / `.global _start` 是第 1 章讲过的伪指令,把后续代码放进 16 位代码段、并把 `_start` 导出给链接器当入口。

入口 `_start` 只有一条指令就远跳:

```asm
_start:
    ljmp $0, $real_start    // 对照表"远转移":ljmp $段, $偏移
```

对照表里"远转移"那行:`ljmp $0, $real_start`。两个 `$`——第一个是段选择子 `0`,第二个是段内偏移 `real_start`。这一跳把 `CS` 钉成 0(段归一化),细节见正文 001。

接着是实模式初始化的标准动作,每一行都能在对照表里查到:

```asm
real_start:
    cli                          // 关中断,无操作数指令,两语法同形
    xorw %ax, %ax                // 对照表"自身异或清零":xorw %ax,%ax(NASM: xor ax,ax)
    movw %cs, %ax                // ← 源 %cs 在左!对照表"寄存器→寄存器"
    movw %ax, %ds                // ds = ax(此时 ax = cs = 0)
    movw %ax, %es
    movw %ax, %ss                // ss 也归零,栈段和代码段对齐
    movw %ax, %fs
    movw %ax, %gs
    cld                          // df 清零,让 lodsb 等串指令向高地址走
    movw $STACK_BASE_ADDR, %sp   // 对照表"立即数→寄存器":$0x7000 → %sp
    sti                          // 栈搭好才开中断
    movb %dl, boot_drive         // 寄存器→内存(直接符号地址),后缀 b = 一字节
    call load_stage2
    movw $msg_booting, %si       // 立即数(标号地址)→ %si
    call print_string_mbr
    ljmp $STAGE2_LOAD_ADDR >> 4, $0   // 远跳到 0x8000(段=0x800)
```

挑三行重点看:

- **`movw %cs, %ax`**——这一行是 Intel→AT&T 翻译最容易写反的地方。Intel 是 `mov ax, cs`(目的 ax 在左),AT&T 源在左,所以 `%cs` 在前、`%ax` 在后。整个"理顺段"的逻辑就是把 `cs` 的值经 `ax` 中转,复制给 `ds/es/ss/fs/gs`,让所有段寄存器都等于 `cs`。这一步省不得,省了就是一屏乱码(正文 001 第 2 节有详细推导)。
- **`movb %dl, boot_drive`**——`boot_drive` 是 [mbr.S:153](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/mbr.S) 定义的一个一字节变量(`boot_drive: .byte 0`)。这里 `boot_drive` 不加 `$`,因为它是个**内存地址**(标号),不是立即数;加 `$` 就变成"把这个标号的地址值写进去"了。后缀 `b` 是因为只搬一字节(`dl` 是 8 位)。这一行把 BIOS 放进 `dl` 的启动盘号存起来,后面读盘要用。
- **`ljmp $STAGE2_LOAD_ADDR >> 4, $0`**——`STAGE2_LOAD_ADDR = 0x8000`,`>> 4` 得 `0x800`,作为段值;偏移 `0`。段 `0x800` << 4 + 偏移 `0` = 物理 `0x8000`,正好是 MBR 把 Stage2 读进来的地方。这一跳交棒给 Stage2。

读盘那段 `load_stage2`([mbr.S:107-135](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/mbr.S))是对照表"带位移的内存"那行的集中演练——在 `0x7B00` 处一个字节一个字段地填 DAP 结构:

```asm
load_stage2:
    movw $DAP_STORE_ADDR, %si        // si 指向 0x7B00(临时 DAP 区)

    movb $DAP_SIZE, (%si)            // 对照表"立即数→内存":字节
    movw $STAGE2_SECTORS, 2(%si)     // 对照表"带位移的内存":2(%si),写一字
    movw $STAGE2_LOAD_ADDR, 4(%si)
    movw $0, 6(%si)
    movl $STAGE2_LBA, 8(%si)         // 注意 l 后缀:写 32 位
    movl $0, 12(%si)
    movb boot_drive, %dl             // 把存好的盘号取回 dl
    movw $0x4200, %ax                // AH=0x42 扩展读
    int $0x13                        // 调 BIOS 磁盘中断
    jc disk_error                    // CF=1 失败
    ret
```

注意 `2(%si)`、`4(%si)`、`8(%si)` 这一串——它们都是对照表里 `disp(base)` 的形式,位移依次是 2/4/8/12,正是 DAP 结构里 sectors/offset/segment/lba 各字段的偏移。`movl $STAGE2_LBA, 8(%si)` 用 `l` 后缀写 32 位,因为 LBA 是 64 位的低 32 位。`int $0x13` 的 `$` 不能少——它是软中断号立即数,少了 `$` 汇编器会把 `0x13` 当成内存地址。

读盘成功,Stage2 就躺在 `0x8000`,MBR 的使命完成,远跳过去。到这一步,我们用对照表读懂了 Cinux 第一段代码的每一行——这正是模块 2 的收官:**看到任何一行 AT&T 都能秒翻成 Intel、反之亦然,且知道它在编译回路里走到哪一步**。

> `mbr.S` 的完整业务逻辑(BIOS 跳转约定、DAP 字段含义、`0x7C00`/`0x8000` 内存布局、栈为什么放 `0x7000`)在正文 [001 · 实模式引导](../../book/01-boot/001-boot-real-mode.md) 逐节讲透,这里不重复。下一章我们离开汇编、进入模块 3 的 C/C++ 内核视角。

---

### 参考

- Intel SDM Vol.3A — §3.4.5「Segment Descriptors」(本仓库本地 PDF `document/reference/intel/SDM-Vol3A-System-Programming-Guide-Part1.pdf`,2023-06 版,第 3-9 ~ 3-11 页,Figure 3-8 描述符布局):段描述符 Base/Limit/Type/S/DPL/P/G/D-B/L 各字段定义与编码。
- GCC 手册 — *Using the GNU Compiler Collection*,汇编选项(`-Wa,--32`、`-ffreestanding`/`-nostdlib`/`-no-pie` 传递):https://gcc.gnu.org/onlinedocs/。
- GAS 文档 — *Using as*,i386 依赖章节(*i386-Syntax* / *i386-Memory* / *i386-Regs*):AT&T 的 `%`/`$` 前缀、源目顺序、`disp(base,index,scale)`、`b/w/l/q` 后缀规则:https://sourceware.org/binutils/docs/as/。
- OSDev — [X86 Assembly](https://wiki.osdev.org/X86_Assembly)、[AT&T Syntax](https://wiki.osdev.org/AT%26T_Syntax)、[GDT Tutorial](https://wiki.osdev.org/GDT_Tutorial)(access byte / flags 速查)。
- 本仓库源码:[mbr.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/mbr.S)、[stage2.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/stage2.S)、[boot/CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/CMakeLists.txt)。

> 参考 URL 的有效性会在全局审查阶段用 open-websearch(bing)统一核活,与本系列其它章节一致。
