---
title: 002 · 进入保护模式
---

# 002 · 进入保护模式:第一张 GDT 与那句 far jump

> 上一章 [001](001-boot-real-mode.md) 我们在实模式里把 BIOS 能蹭的服务都蹭完了:读盘、开 A20、配 VESA、存 framebuffer 参数。这一章,我们要和实模式告别——建出 Cinux 的第一张 GDT,拨动 `CR0` 上的一个开关,再用一句远跳,让 CPU 真正跨进 32 位保护模式。跨过去之后,BIOS 就再也叫不应了。

## 这一章我们要点亮什么

001 的 Stage2 在配完 VESA 之后是直接 `hlt` 死等。002 在那个位置接上一段新代码,把机器从实模式切到保护模式:

```text
... (VESA 配屏,同 001) ...
 └─▶ cli                  # 全程关中断:没有 IDT,开了中断就三重故障
      └─▶ DS = 0          # 给 lgdt 一个干净的寻址基址
      └─▶ lgdt gdt_ptr    # 把我们的 GDT 告诉 CPU
      └─▶ CR0 |= 0x1      # 拨动 PE(Protection Enable)位
      └─▶ ljmp $0x08, $pm_entry   # 远跳:刷新 CS,正式进入 32 位 PM
           └─▶ pm_entry (.code32)
                ├─ DS=ES=FS=GS=SS = 0x10   # 装载新的数据段选择子
                ├─ ESP = 0x90000           # 保护模式下的新栈
                ├─ outb 'P', $0xE9         # 往 debugcon 吐一个 'P'
                └─ hlt 循环
```

完成后你会看到:QEMU 窗口里 001 那几行文本(`Stage2 OK`、`Mode info OK, switching...`)照常出现,屏幕切进图形模式;然后——因为进了 PM、屏幕是图形、BIOS 也没了——没法再用 `INT 0x10` 打印。于是我们改用一个新的输出手段:往端口 `0xE9` 写一个字节,QEMU 会把它记进 `build/debug.log`。看到 `debug.log` 里出现一个 `P`,就说明保护模式这条链真的走通了。

## 为什么现在需要它

实模式有两个绕不开的天花板。第一,**只能寻址 1MB**(`段<<4 + 偏移`,段 16 位、偏移 16 位,理论 1MB+64KB),现代内核要管几百兆内存,这点空间塞牙缝都不够。第二,**段式寻址**那套 `段<<4+偏移` 又啰嗦又没有保护——任何程序都能写任何地址,一个野指针就能把别人(或内核自己)写花。

保护模式把这两件事一起解决:**地址翻译改成"段选择子 → 查 GDT → 得到段基址和限长"**,而且每个段带访问权限(可读/可写/特权级),CPU 会检查——你拿数据段选择子去当代码执行,CPU 直接给你一个异常。要管大内存,我们用一张"扁平"的 GDT:段基址全设成 0、限长设成 4GB,于是"段"这个抽象基本透明,地址就等于线性地址——既绕开了 1MB 天花板,又为后面的分页打好了底。

那为什么是"现在"切?因为切过去之前,所有要用 BIOS 的活必须先干完(见 001 的解释)。001 把 VESA、A20 都做完了,这一章正好是离别的时刻。

> 外部依据:Intel SDM Vol.3A §9.9 给出了从实模式切换到保护模式的标准步骤(建 GDT → `cli` → `lgdt` → 置 `CR0.PE` → 远跳刷新 CS → 刷新段寄存器);OSDev 的 Protected Mode / Global Descriptor Table 页对扁平模型有社区视角的总结。

## 设计图

先看这张 GDT 长什么样。它是内存里一段连续的 8 字节条目,前面三项就够我们进 PM:

```text
偏移   entry           选择子    access  base/limit        用途
0x00   [ null      ]    —        0x00    base=0,lim=0       第 0 项必须全 0
0x08   [ code      ]    0x08     0x9A    base=0,lim=4GB     32 位代码段(可执行/可读)
0x10   [ data      ]    0x10     0x92    base=0,lim=4GB     32 位数据段(可读写)
```

选择子的规则和 [010](../03-big-kernel/010-big-kernel-gdt.md) 那张 GDT 是同一套:`选择子 = (条目偏移) | RPL`,RPL=0 不加。所以 `0x08` = 第 1 项、`0x10` = 第 2 项。

注意:这张 GDT 是 **bootloader 的最小 PM GDT,base 全是 0 的扁平模型**。别和后面 big kernel(010)那张 7 项、带 TSS、带用户段的完整 GDT 搞混——那是内核自己后来重建的。这里我们只要"刚好够进 PM"。

再看实模式 → 保护模式的**状态机**,每一步都不可逆,顺序错了就是三重故障:

```text
实模式(16 位,DS<<4+偏移)
  │  cli                 # 关中断(全程不开,没有 IDT)
  │  DS=0                # 让 lgdt 的实模式寻址算对
  │  lgdt gdt_ptr        # 把 GDT 基址/限长装进 GDTR(CPU 此刻还不校验内容)
  │  CR0.PE = 1          # 拨开关——但 CPU 还在用旧的 CS/16 位译码!
  ▼
  ljmp $0x08, $pm_entry  # ← 关键的远跳:强制用新 GDT 重载 CS,切到 32 位译码
  │
  ▼
保护模式(32 位,扁平)
  │  DS=ES=FS=GS=SS = 0x10   # 装载新数据段
  │  ESP = 0x90000           # 新栈
```

## 代码路线

源码主要在 [stage2.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/stage2.S)(VESA 之后的 PM 切换序列 + GDT 定义)和 [CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/CMakeLists.txt)(链接地址的改动)。

### 1. GDT:用一张扁平表取代段式寻址

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

> 这里有个**源码注释和实现不符**的地方,值得拎出来说:`gdt_code` 的 `.word 0x0000` 那行源码注释写着 "Base 15:0 (= 0x8000)",但实际编码出来的 base 是 **0**,不是 0x8000。这是对的——扁平模型必须 base=0,否则进 PM 后 `CS` 基址是 0x8000,而 `pm_entry` 又是按 0x8000 链接的绝对地址,两者一加就错位崩了。注释是笔误,代码是正确的。读这段源码时别被注释带偏。(这和 [010](../03-big-kernel/010-big-kernel-gdt.md) 里 TSS 注释写成 "Table 8-2" 是同一类问题——源码注释是线索,不是权威。)

`gdt_ptr` 是给 `lgdt` 用的 6 字节结构(16 位 limit + 32 位 base):

```asm
gdt_ptr:
    .word (gdt_end - gdt - 1)      # limit = 表长 - 1
    .long gdt                      # base = GDT 的线性地址
```

`gdt_end - gdt - 1 = 23`(3 项 × 8 − 1),`gdt` 这个标号经链接后是它在 `0x8000` 之后的绝对地址。

### 2. lgdt 与"为什么实模式要先 DS=0"

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

> 外部依据:Intel SDM Vol.3A §3.4.4(LGDT/GDTR 结构)、§9.9.1(切换到 PM 前的 GDTR 装载)。`lgdt` 本身只搬运那 6 个字节,**不校验 GDT 内容合法性**——合法性要到后续真正用某个段选择子时才查,这点很容易踩(见调试现场)。

### 3. CR0.PE:拨动那一个开关

```asm
movl %cr0, %eax
orb $0x1, %al          # 置 bit 0 = PE
movl %eax, %cr0        # 写回 CR0
```

`CR0` 的 bit 0 叫 **PE(Protection Enable)**。置 1 的这一刻,CPU "名义上"已经是保护模式了。但注意:**置位之后,CPU 仍然在用旧的 `CS`、旧的 16 位译码方式执行**——它不会自动刷新。这就埋下了下一节的那个关键动作。

全程 `cli` 不是可有可无。我们此刻**没有 IDT**(那是后面 big kernel 的事),一旦允许中断,任何异步中断(比如 PIT 定时器)进来找不到处理程序,直接三重故障重启。所以从 `cli` 到 `pm_entry` 之间,中断必须一直关着。

### 4. 远跳:不刷新 CS 就不算真正进入 PM

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

### 5. pm_entry:新的段、新的栈,还有 0xE9 debugcon

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

`outb %al, $0xE9` 是这一章新引入的输出手段。QEMU 的 **debugcon** 设备挂在端口 `0xE9`,往它写一个字节,QEMU 就把字节记到一个文件里([qemu.cmake](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/cmake/qemu.cmake) 里配了 `-debugcon file:debug.log -global isa-debugcon.iobase=0xe9`)。为什么需要它?因为进 PM 后 `INT 0x10` 没了(告别 BIOS),屏幕又是 VESA 图形模式(没字体、不能 teletype),我们陷入了"既没 BIOS、又没屏幕、又没串口"的输出真空。debugcon 是这个真空期里最便宜的可观测手段——写一个 `P` 到 `build/debug.log`,就知道 `pm_entry` 真的执行到了。注意它**不是真串口**(串口是 COM1/端口 `0x3F8`,驱动要等 [012](../03-big-kernel/012-kprintf-sse.md)),只是个 QEMU 专用的调试后门。

## 调试现场

进 PM 这一段是 Cinux 踩坑最密集的地方,因为太多东西(寻址模型、译码宽度、CS 刷新)要在几条指令内一起转过来。下面是几个真实调出来的。

**症状一**——`lgdt` 之后莫名其妙崩。 几乎都是 `DS` 没清零。实模式 `lgdt` 按 `DS<<4+偏移` 取 GDTR 地址,`DS` 还是脏的就读到错内存。修复就是 `lgdt` 前那两行 `movw $0,%ax; movw %ax,%ds`。判断:GDB 里在 `lgdt` 前后看 `GDTR`(`info registers` 或 `monitor`),limit 应该是 23、base 应该落在 `0x81xx`;要是 base 一眼不对,就是 `DS` 的问题。

**症状二**——置了 `CR0.PE`,程序原地三重故障重启。 多半是漏了 far jump,或者 far jump 的编码不对。置 PE 之后 CPU 仍在 16 位译码,没有远跳刷新 `CS`,后面那条 `.code32` 编码的指令被当 16 位解码,几条就崩。修复就是老老实实 `ljmp $0x08, $pm_entry`。**别手拼机器码**(`ea <off16> <seg16>`)——GAS 在 `.code16` 下会自动生成正确的 16 位远跳编码,手拼反而容易错。

**症状三**——GDB 报 "Invalid register `ip`",或者反汇编出一堆 `(bad) + rex`。 这是经典的"译码宽度对不上"。要么是 `.code16`/`.code32` 放错了位置,要么是你给 GDB 喂的是 `stage2.bin`(裸二进制,没符号、没段信息)而不是 `stage2`(ELF)。**调试一律用 ELF**(`file build/boot/stage2`),`bin` 是给启动加载用的,两者不能互相替代。`ip`(实模式)变 `eip`(PM)的切换点正好在 far jump,跨过这条线 GDB 的寄存器名会变,这也是判断"是否真的进了 PM"的一个旁证。

**症状四**——GDT 看着填了,但一访问段就 #GP。 `lgdt` 不校验 GDT 内容,错要等到用选择子时才暴露。常见是 access byte 某一位算错(比如把代码段的可执行位弄没了),或者 limit 算成 `gdt_end - gdt`(忘了 `-1`)。对着 Intel SDM 的段描述符位定义再核一遍 base/limit/access/flags 四组字节,别凭感觉。

## 验证

第一道闸是构建。和 001 一样,002 没有 host 侧自动化测试,构建本身就是冒烟:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -S .
cmake --build build -j$(nproc)
```

`build/boot/stage2.bin` 能产出、`cinux.img` 能拼好,就过。

第二道闸是跑起来分两段看。`cmake --build build --target run`:

- **切 PM 之前**(还在实模式):看 QEMU 窗口,001 那几行文本(`Stage2 OK`、`Mode info OK, switching...`)照常出现,屏幕切进图形模式。这段和 001 完全一样。
- **切 PM 之后**(没 BIOS、屏幕是图形):没有任何屏幕/串口输出。这时候去看 **`build/debug.log`**——里面应该有一个 `P`(我们 `outb` 到 `0xE9` 的)。有 `P`,就证明 `pm_entry` 执行到了,保护模式切换成功。

```bash
cat build/debug.log    # 期望看到 'P'(可能还有点尾部噪声)
```

第三道闸是用 GDB 确认模式真的切了。`cmake --build build --target run-debug` 起带 `-s -S` 的 QEMU,另一终端:

```text
(gdb) file build/boot/stage2          # 用 ELF,别用 bin
(gdb) target remote :1234
(gdb) b *pm_entry
(gdb) continue
# 命中断点说明 far jump 成功;此时 info registers 应是 32 位(eip/eax 等)
(gdb) info registers eflags           # 看 VM/RF 位,确认已不在 V8086/实模式
```

能停在 `pm_entry`、寄存器名从 `ip` 变 `eip`,就是实打实地进了 32 位保护模式。

## 下一站

我们现在是 32 位保护模式,有一张扁平 GDT、一个能跑的栈、一个 debugcon 后门。可 x86_64 的故事在 64 位——32 位 PM 只是个中转站。要进 64 位长模式,得先建一套**分页**(因为长模式强制要求分页开启),把 `CR4.PAE`、`EFER.LME`、`CR0.PG` 一个个拨起来,然后再来一次远跳,带着一个 L 位=1 的 64 位代码段选择子。

下一章 [003 · 长模式](003-boot-long-mode.md),我们就在这张 32 位 PM 的地基上,把分页和长模式搭起来,让 Cinux 真正变成 64 位。那张 64 位的 GDT,以及后面 big kernel 自己重建的完整 GDT,都是后话——现在我们只需要这刚刚够用的三行。

---

### 参考

- Intel SDM Vol.3A — §3.4.2 Segment Descriptors(描述符格式与位定义)、§3.4.4 GDTR/LGDT、§3.5 控制寄存器(CR0 与 PE 位)、§9.9 Switching to Protected Mode(标准步骤、far jump 要求、`cli` 时机)。
- OSDev — [Global Descriptor Table](https://wiki.osdev.org/Global_Descriptor_Table)(扁平模型的最小 GDT)、[Protected Mode](https://wiki.osdev.org/Protected_Mode)。
- OSDev — [Debugcon](https://wiki.osdev.org/Debugcon)(QEMU 端口 `0xE9` 调试后门)。
- 本 tag 源码:[stage2.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/stage2.S)(PM 切换序列与 GDT)、[serial.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/common/serial.S)(pushw 化)、[CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/CMakeLists.txt)(链接地址 `0x8000`、`.gdt` 段)、[qemu.cmake](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/cmake/qemu.cmake)(debugcon)。
- 调试素材提炼自 [1.md](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/document/notes/002/1.md) 与 [2.md](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/document/notes/002/2.md)。

> Intel SDM 版本说明:本卷引用的 SDM 章节号沿用较早版本编号。若按项目本地 PDF(`document/reference/intel/`,2023-06 版)查阅,部分内容已重排——段描述符在 §3.4.5、GDTR/LGDT 在 §2.4.1、控制寄存器(CR0/PE)在 §2.5、切换到保护模式在 §10.9。以章节标题为准,别拘泥于编号。

> 参考 URL 的有效性会在全局审查阶段用 open-websearch(bing)统一核活。
