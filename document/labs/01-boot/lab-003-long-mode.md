---
title: Lab 003 · 跨进长模式
---

# Lab 003 · 跨进长模式

> 这个 lab 配套 [003 · 跨进长模式](../../book/01-boot/003-boot-long-mode.md)。目标是在 [Lab 002](lab-002-gdt-protected.md) 的 `pm_entry`(打完 `'P'`)之后接上:搭一套临时页表、按固定顺序拨开关、远跳进 64 位,最后 debugcon 再吐一个 `'L'`。**页表项自己算位、EFER 自己拨、64 位描述符自己拼**,不给现成答案。

## 实验目标

- 实现 `setup_page_tables`:在 `0x1000/0x2000/0x3000` 清零三张表、串起三级指针、用 4 个 2MB 大页恒等映射前 8MB。
- 实现 `enter_long_mode`:严格按 `CR3`→`CR4.PAE`→`EFER.LME`→`lgdt`→`CR0.PG`→远跳的顺序。
- 扩展 GDT 加 64 位代码段(L=1)和数据段,并准备 `gdt64_ptr`。
- 在 `.code64 long_mode_entry` 里设段、设栈、往 `0xE9` 打 `'L'`。
- 验证:`build/debug.log` 出现 `PL`。

## 前置条件

- 已完成 Lab 002:机器能进 32 位 PM,`debug.log` 里能看到 `P`。
- 理解四级页表(PML4/PDPT/PD/PT)、2MB 大页的 PS 位、MSR 的读写(`rdmsr`/`wrmsr`)。不清楚就先翻 [003 章的设计图](../../book/01-boot/003-boot-long-mode.md#设计图)。

## 任务分解

分四块走。

### 第一块:setup_page_tables

在 [long_mode.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/common/long_mode.S) 写 `setup_page_tables`:先 `cld`,用 `rep stosl` 把 `0x1000`/`0x2000`/`0x3000` 各清零 4096 字节(1024 个 dword);然后 `PML4[0]=0x2003`、`PDPT[0]=0x3003`;最后循环填 `PD[0..3]`,`entry = (i << 21) | 0x83`(PS 位 = 大页)。

想清楚 `0x83` 的含义:`Present | Writable | Large`,少了 Large 位 CPU 就会去查不存在的 PT。

### 第二块:enter_long_mode 序列

照固定顺序:`CR3 = 0x1000` → `CR4 |= 0x20`(PAE)→ `EFER(MSR 0xC0000080) |= 0x100`(LME,用 `rdmsr`/`wrmsr`)→ `lgdt gdt64_ptr` → `CR0 |= 0x80000001`(PG|PE,用 `orl` 别用 `movl`)→ `ljmp $0x18, $long_mode_entry`。

记住 `EFER.LME` 设了不立刻生效,要等 `CR0.PG` 置位那拍才激活——这是顺序不能乱的根本原因。

### 第三块:扩展 GDT + gdt64_ptr

在 [stage2.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/stage2.S) 的 GDT 里,在 null/code32/data32 后加两项:`gdt_code64 = .quad 0x00AF9A000000FFFF`(L=1,D=0)、`gdt_data64 = .quad 0x008F92000000FFFF`。再写 `gdt64_ptr`:limit = 表长−1(现在 5 项,自己算),base 用 `.long gdt` + `.long 0` 拼成 64 位(别用 `.quad`,会触发 32 位 ELF 的 64 位重定位)。

### 第四块:long_mode_entry

[stage2.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/stage2.S) 里 `.code64 long_mode_entry:`:数据段全设 `0x20`,`movabsq $0x90000, %rsp`,`movb $0x4C,%al; outb %al,$0xE9` 打 `'L'`,然后 `cli; hlt` 循环。在 `pm_entry` 末尾(打完 `'P'`)接上 `call setup_page_tables` 和 `call enter_long_mode`。

## 接口约束

这些得自己保证对、lab 不给现成代码:

- **页表地址**:PML4 `0x1000`、PDPT `0x2000`、PD `0x3000`,各 4096 字节,先清零。
- **页表项标志**:中间层 `0x03`(P|R|W),大页项 `0x83`(P|R|W|Large);大页基址 `i << 21`。
- **MSR**:EFER 地址 `0xC0000080`,LME = bit 8 = `0x100`;读写用 `rdmsr`/`wrmsr`,`ecx` 放地址、`edx:eax` 放数据。
- **CR 位**:`CR4.PAE = 0x20`、`CR0.PG = 0x80000000`、`CR0.PE = 0x01`;改 CR0/CR4 一律 `orl` 不要 `movl`。
- **选择子**:64 位代码 `0x18`、64 位数据 `0x20`。
- **64 位代码描述符**:`0x00AF9A000000FFFF`,flags 高 nibble 必须是 `0xA`(G=1, D=0, **L=1**);L=1 时 D 必须 0。
- **gdt64_ptr**:base 用 `.long`+`.long` 拼,不用 `.quad`。
- **全程 `cli`**:没有 IDT,任何中断三重故障。

## 验证步骤

**第一道:构建冒烟。**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -S .
cmake --build build -j$(nproc)
```

**第二道:debugcon。** `cmake --build build --target run`:

```bash
cat build/debug.log    # 期望:PL(P=002 进 PM,L=003 进长模式)
```

**第三道:GDB。** `cmake --build build --target run-debug`,另一终端:

```text
(gdb) file build/boot/stage2
(gdb) target remote :1234
(gdb) b *long_mode_entry
(gdb) c
(gdb) p/x $cs                 # 期望 0x18
```

停在 `long_mode_entry`、`cs=0x18`、EFER.LMA=1,就是真的进了 64 位。

## 常见故障

**`CR0.PG` 一置位就三重故障。** 页表不对:表没清零、大页项漏了 Large(PS)位、或某层指针写错地址。置 PG 前用 GDB `x/4gx 0x3000` 看 PD 项是不是 `0x...83`,`x/1gx 0x1000` 看 PML4[0] 是 `0x2003`。

**置 EFER.LME 或开 PG 时 #GP。** 顺序错了。必须 PAE→LME→PG,且 PAE 在 LME 之前。对着章里的状态机核。

**远跳进 long_mode_entry 后崩。** 64 位代码描述符 L/D 位错。flags 高 nibble 要是 `0xA`(G=1,D=0,L=1),不是 `0xC`。

**链接报 64 位重定位错。** `gdt64_ptr` 用了 `.quad`。改 `.long gdt; .long 0`。

**栈一压就崩。** 忘了在 long_mode_entry 里把 `SS` 刷成 `0x20`(长模式 SS 也得是有效段),或 `rsp` 没用 `movabsq` 装 64 位值。

## 通过标准

- `cmake --build build` 成功,`stage2.bin`(含 `.code64`)产出。
- `make run` 后 `build/debug.log` 出现 `PL`。
- GDB 能断在 `long_mode_entry`、`cs=0x18`、EFER.LMA 置位。
- 全程 `cli`、没有 IDT;页表只是临时恒等映射,没碰真正的 PMM/VMM——那些是后面的 lab。
