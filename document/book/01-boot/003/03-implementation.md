---
title: 03 · 代码路线:setup_page_tables、enter_long_mode、扩展 GDT
---

# 代码路线:setup_page_tables、enter_long_mode、扩展 GDT

源码主要在新增的 [long_mode.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/common/long_mode.S)(`setup_page_tables` 和 `enter_long_mode`)以及 [stage2.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/stage2.S) 末尾接上的 `.code64 long_mode_entry` 和扩展 GDT。

## 1. 为什么长模式必须先有分页

(上面"为什么现在需要它"已经讲了原因,这里补一个实操上的关键点。)我们待会儿要 `lgdt`、要远跳、要读内存里的页表本身——这些地址翻译,在分页开启后全部要走我们搭的这套页表。所以**页表必须先搭好、并且正确**,否则 `CR0.PG` 一置位,CPU 连下一条指令的地址都翻译不出来,当场三重故障。这就是为什么 `setup_page_tables` 是第一件事,而且要做成恒等映射:让"搭页表的代码所在的地址"在分页前后都指向同一处,避免"开了分页反而找不到自己"的尴尬。

## 2. setup_page_tables:三张表 + 4 个 2MB 大页

[long_mode.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/common/long_mode.S) 里,先把三张表清零(页表项未用的位必须是 0,否则 CPU 当成有效项去查,会出问题):

```asm
setup_page_tables:
    cld
    # 清零 PML4(0x1000)/ PDPT(0x2000)/ PD(0x3000),各 1024 个 dword = 4096 字节
    movl $0x1000, %edi
    xorl %eax, %eax
    movl $1024, %ecx
    rep stosl                # ... 对 0x2000、0x3000 同样再来两遍
```

清零靠 `rep stosl`——`ecx` 个 dword、从 `edi` 起逐个写 `eax`(0),一个循环写完一整页。这里 `cld` 先把方向标志清零,保证 `stosl` 是地址递增方向(否则往低地址写,直接写飞)。

然后串起三级指针,再填大页:

```asm
    # PML4[0] → PDPT,带 present+writable
    movl $0x2000, %eax
    orl  $0x03, %eax         # 0x03 = Present | Writable
    movl %eax, 0x1000        # 写进 PML4[0]

    # PDPT[0] → PD
    movl $0x3000, %eax
    orl  $0x03, %eax
    movl %eax, 0x2000        # 写进 PDPT[0]

    # PD[0..3]:4 个 2MB 大页,恒等映射 0~8MB
    movl $0x3000, %edi
    movl $4, %ecx
    xorl %eax, %eax          # i = 0
1:
    movl %eax, %edx
    shll $21, %edx           # 物理基址 = i << 21(每页 2MB = 0x200000)
    orl  $0x83, %edx         # 0x83 = Present | Writable | Large(PS 位)
    movl %edx, (%edi)
    addl $8, %edi            # 下一项(每项 8 字节)
    incl %eax
    loop 1b
    ret
```

这里每一层的细节:

- **`0x03 = Present(0x01) | Writable(0x02)`**:中间层(PML4/PDPT)的项指向下一层表,只需要这两个权限。
- **`0x83 = Present | Writable | Large(0x80)`**:`Large` 位(页表项里的 PS 位,bit 7)是关键——它告诉 CPU"这一项不是指向下一层 PT 的指针,它本身就是一个大页"。置了它,CPU 到 PD 这层就停下,直接用这一项的基址当 2MB 页的起始。没置 PS 位,CPU 会继续去查一个根本不存在的 PT,读到 0,触发缺页。
- **`i << 21`**:2MB = `0x200000` = `1 << 21`。第 `i` 个大页的物理基址就是 `i << 21`。恒等映射下,虚拟基址也是 `i << 21`,所以前 8MB 虚拟地址 = 物理地址。

每个页表项 8 字节(64 位),但因为我们只用到低 32 位(地址都在 4GB 以内),代码里用 32 位写(`movl`)只写了低 4 字节,高 4 字节是前面清零留下的 0——对低地址映射来说够了。

## 3. enter_long_mode:顺序即一切

[long_mode.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/common/long_mode.S) 的 `enter_long_mode` 就是上面设计图里那串状态机的直译,顺序一个都不能动:

```asm
enter_long_mode:
    movl $0x1000, %eax
    movl %eax, %cr3              # ① CR3 = PML4 基址

    movl %cr4, %eax
    orl  $0x20, %eax             # CR4.PAE = bit 5
    movl %eax, %cr4              # ② 开 PAE

    movl $0xC0000080, %ecx       # EFER 的 MSR 地址
    rdmsr                        # 读 EFER 到 edx:eax
    orl  $0x100, %eax            # EFER.LME = bit 8
    wrmsr                        # ③ 写回 EFER(此刻 LME 还没生效)

    lgdt gdt64_ptr               # ④ 换带 64 位段的 GDT

    movl %cr0, %eax
    orl  $0x80000001, %eax       # CR0.PG(bit 31) | CR0.PE(bit 0)
    movl %eax, %cr0              # ⑤ ★ 开分页:LME 激活,长模式生效

    ljmp $0x18, $long_mode_entry # ⑥ 远跳到 64 位代码段
```

这里有四处必须留意,逐个过一遍。`EFER` 是个 MSR(Model-Specific Register),地址 `0xC0000080`,不能用 `mov`,得用 `rdmsr`/`wrmsr`——读时结果落在 `edx:eax`、写时也从 `edx:eax`,操作前把地址放进 `ecx`,而 `LME` 是 bit 8,即 `0x100`。顺序则是死的:PAE(`CR4`)必须在 `EFER.LME` 之前、`EFER.LME` 必须在 `CR0.PG` 之前,`CR0.PG` 置位那一拍长模式才真正激活,这就是 Intel 的固定序列(详见 SDM §9.8.1.1)。`CR0 |= 0x80000001` 这步同时置 PG(bit 31)和保留 PE(bit 0),注意用 `orl` 而非 `movl`——`CR0` 里还有别的控制位(比如 cache 相关),直接 `movl $...` 会把它们清掉,这和 002 置 PE 时用 `orb` 是一个道理。最后还是那条远跳:`CR0.PG` 置位后 CPU 已在长模式,可 `CS` 还指向 32 位段,和 002 进 PM 时一样,必须一条远跳带着新的 64 位代码段选择子(`0x18`)去刷新 `CS`,而紧跟的 `.code64` 则告诉汇编器从 `long_mode_entry` 起按 64 位编码。

## 4. 扩展 GDT:64 位代码段的关键是 L 位

长模式需要一个 **L 位 = 1** 的代码段描述符。我们在 [stage2.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/stage2.S) 的 GDT 里,在 002 那三项(null/code32/data32)后面又加了两项:

```asm
gdt_code64:
    .quad 0x00AF9A000000FFFF     # 64 位代码段:L=1, D=0
gdt_data64:
    .quad 0x008F92000000FFFF     # 64 位数据段
```

把 `0x00AF9A000000FFFF` 按小端拆成字节看:`FF FF 00 00 00 9A AF 00`。关键的两个字节:

- `access = 0x9A`(`1001 1010`):P=1、DPL=0、S=1、code/exec/read——和 32 位代码段一样。
- `byte[6] = 0xAF`:高 4 位是 flags `1010`——**G=1、D/B=0、L=1**。这里的 `L=1` 就是"长模式代码段"的标志;同时 `D=0`(在 L=1 时 D 必须为 0,这是 Intel 的规定,否则触发 #GP)。低 4 位 `0xF` 是 limit 19:16。

选择子也相应扩出来:`0x08`/`0x10` 还是 32 位那两个(002 已用),新增 `0x18` = 64 位代码、`0x20` = 64 位数据。GDT 从 3 项变 5 项。

`gdt64_ptr` 是给长模式 reload 用的 GDTR。这里有个 ELF 的小坑:Stage2 是按 32 位 ELF(`elf_i386`)链接的,如果直接用 `.quad gdt` 写 64 位 base,会触发一个 32 位 ELF 不支持的 64 位重定位。所以代码用 `.long gdt` + `.long 0` 两段拼出 64 位 base——GDT 在低地址,高 32 位是 0,这样既绕开了重定位,又给出了正确的 64 位基址。

> 还是要提醒:这张 5 项 GDT 仍是 **bootloader 的**。后面 big kernel(010)会建它自己完整的 GDT(带 TSS、带用户段)。两者的选择子数值虽然部分重合(都有 0x08/0x10),但不是同一张表。读到这里别把它们混为一谈。

## 5. long_mode_entry:64 位段、64 位栈,debugcon 打 'L'

```asm
.code64
.global long_mode_entry
long_mode_entry:
    movw $0x20, %ax            # 64 位数据段选择子
    movw %ax, %ds              # ... es/fs/gs/ss 同样
    movabsq $0x90000, %rsp     # 64 位栈指针
    movb $0x4C, %al            # 'L'
    outb %al, $0xE9            # debugcon 打 'L'
    cli
.lm_halt:
    hlt
    jmp .lm_halt
```

进了长模式,段寄存器重新刷成 `0x20`(其实长模式下数据段的 base/limit 基本被忽略,但 `SS` 必须是有效段,否则压栈会 #GP)。`rsp` 用 `movabsq` 装一个 64 位立即数(长模式栈用 64 位 `rsp`,不是 32 位的 `esp`)。最后往 `0xE9` 吐一个 `'L'`——和 002 的 `'P'` 用的是同一个 debugcon 机制。
