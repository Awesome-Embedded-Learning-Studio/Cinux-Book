---
title: Lab 002 · 进入保护模式
---

# Lab 002 · 进入保护模式

> 这个 lab 配套 [002 · 进入保护模式](../../book/01-boot/002-boot-gdt-protected.md)。目标是在 [Lab 001](lab-001-real-mode.md) 的 Stage2 末尾(VESA 之后)接上一段代码,把机器从实模式切到 32 位保护模式,并用 0xE9 debugcon 验证自己真的进去了。**GDT 自己拼位、CR0 自己拨、远跳自己写**,不给现成答案。

## 实验目标

- 写一张 3 项的扁平 GDT(null / code / data,base=0,limit=4GB)和配套的 `gdt_ptr`。
- 在实模式里 `lgdt`、置 `CR0.PE`、远跳,正式进入 32 位 PM。
- 在 `pm_entry` 里装载新段选择子、换栈,并往 `0xE9` 吐一个 `P`。
- 验证:`build/debug.log` 里出现 `P`。

## 前置条件

- 已完成 Lab 001:Stage2 能配好 VESA、屏幕能切图形模式。
- 理解段描述符的位布局(access byte / flags nibble / base / limit 三段拆分),不清楚就先翻 [002 章的设计图](../../book/01-boot/002-boot-gdt-protected.md#设计图)。

## 任务分解

分四块走。

### 第一块:写 GDT 和 gdt_ptr

在 [stage2.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/stage2.S) 末尾开一个 `.section .gdt,"a"` + `.align 8`。填三项:`.quad 0` 空段;代码段(limit `0xFFFF`、base 三段全 0、access `0x9A`、flags+limit高 `0xCF`);数据段(同样,access 换成 `0x92`)。再写 `gdt_ptr`:16 位 limit(表长 − 1,自己算该是多少)、32 位 base(`gdt` 标号)。

注意 [CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/CMakeLists.txt) 里 Stage2 的链接脚本要放一个 `.gdt ALIGN(8)` 段——GDT 必须 8 字节对齐。

### 第二块:DS=0 再 lgdt

在 VESA 配屏代码之后、原来 `hlt` 的位置接上:`cli` → `DS=0` → `lgdt gdt_ptr`。想清楚为什么必须先 `DS=0`(实模式寻址是 `DS<<4+偏移`,不归零 `lgdt` 会读错地址)。

### 第三块:CR0.PE + 远跳

`movl %cr0,%eax; orb $0x1,%al; movl %eax,%cr0` 置 PE 位;紧接着 `ljmp $0x08, $pm_entry`。这条远跳必须在 `.code16` 区段里(那时 CPU 还在 16 位译码),别手拼 `ea` 机器码——交给 GAS。

### 第四块:pm_entry 里收拾干净

`.code32 pm_entry:`:把 `DS=ES=FS=GS=SS` 全设成 `0x10`,`ESP` 设成 `0x90000`,`movb $0x50,%al; outb %al,$0xE9` 吐个 `P`,然后 `cli; hlt` 循环。

## 接口约束

这些得自己保证对、但 lab 不给现成代码:

- **选择子**:代码 `0x08`(第 1 项)、数据 `0x10`(第 2 项),RPL=0。
- **描述符字节**:access 代码 `0x9A`、数据 `0x92`;flags+limit高 `0xCF`(G=1、D=1、limit19:16=0xF);base 三段全是 0(扁平模型,**不是**源码注释里写的 0x8000——核一下你写的 base 是不是 0)。
- **CR0.PE**:bit 0,用 `orb $0x1` 而不是 `movl $1`(后者会把 CR0 其它位清零,后果不可预期)。
- **链接地址**:Stage2 必须 `. = 0x8000`(=载入地址),这是绝对寻址/lgdt 的前提。从 001 的 `. = 0x0` 改过来。
- **全程 `cli`**:从 `lgdt` 到 `pm_entry` 之间不许 `sti`——没有 IDT,开中断必三重故障。
- **段宽度**:实模式段一律 `pushw`/`popw`,别混 `pushl`;16 位 `call`/`ret`。
- **0xE9**:是 QEMU debugcon(写进 `build/debug.log`),不是串口。

## 验证步骤

**第一道:构建冒烟。**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -S .
cmake --build build -j$(nproc)
```

**第二道:QEMU 两段看。** `cmake --build build --target run`:

- 切 PM 前:QEMU 窗口里 001 那几行文本照常出现,屏幕切图形(和 001 一样)。
- 切 PM 后:屏幕/串口都没输出,去看 `build/debug.log`:

```bash
cat build/debug.log    # 期望有个 'P'
```

有 `P` 就说明 `pm_entry` 跑到了,PM 切换成功。

**第三道:GDB 旁证。** `cmake --build build --target run-debug`,另一终端:

```text
(gdb) file build/boot/stage2          # 用 ELF,别用 bin
(gdb) target remote :1234
(gdb) b *pm_entry
(gdb) c
```

停在 `pm_entry`、寄存器名从 `ip` 变 `eip`,就是真进了 32 位 PM。

## 常见故障

几个几乎必踩的:

**`lgdt` 后就崩。** `DS` 没清零,`lgdt` 从错地址读了 GDTR。`lgdt` 前补 `DS=0`。

**置了 PE 就三重故障重启。** 漏了远跳,或远跳手拼机器码拼错。CPU 还在 16 位译码,后面的 32 位指令被错解。老实写 `ljmp $0x08, $pm_entry`,别手拼。

**GDB 报 `Invalid register ip` / 反汇编一堆 `(bad)`。** 译码宽度对不上,或喂了 `bin` 没喂 ELF。`file build/boot/stage2` 用 ELF;检查 `.code16`/`.code32` 是不是放对了远跳两侧。

**一访问段就 #GP。** 描述符位算错了。`lgdt` 不校验内容,错要到用选择子时才爆。对着 SDM 段描述符位定义核 base/limit/access/flags,limit 记得是表长 − 1。

**栈一压就炸。** 忘了在 `pm_entry` 里换栈,`ESP` 还是实模式遗留的小值。补 `movl $0x90000, %esp`,并把 `SS` 刷成 `0x10`。

## 通过标准

- `cmake --build build` 成功,`stage2.bin` 产出。
- `make run` 后:QEMU 窗口先显示 001 的文本、切图形;`build/debug.log` 里出现 `P`。
- GDB 能断在 `pm_entry`、寄存器变 32 位。
- 全程没有 `sti`、没有 IDT、没有碰任何 paging/PAE/EFER——那是 [Lab 003](lab-003-long-mode.md) 的事。
