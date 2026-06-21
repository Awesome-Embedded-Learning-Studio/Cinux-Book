---
title: 003 · 跨进长模式
---

# 003 · 跨进长模式:临时页表与那句 64 位远跳

> [002](002-boot-gdt-protected.md) 我们把机器拉进了 32 位保护模式,打了个 `P` 到 debugcon 就停了。可 Cinux 是个 x86_64 系统,真正要跑的是 64 位。这一章,我们就在那张 32 位 PM 的地基上,搭一套**临时分页**、按 Intel 规定的固定顺序拨开几个开关,再用一句远跳跨进 64 位长模式——跨过去之后,debugcon 会再吐一个 `L`。

## 这一章我们要点亮什么

在 `pm_entry` 打完 `'P'` 之后,我们不再 `hlt`,而是接着干两件事,把 CPU 从 32 位 PM 推进 64 位长模式:

```text
pm_entry (32 位 PM)
 └─▶ setup_page_tables()     # 在 0x1000/0x2000/0x3000 搭临时恒等映射
 └─▶ enter_long_mode()
      ├─ CR3 = 0x1000        # 装载 PML4 基址
      ├─ CR4 |= PAE          # 开物理地址扩展
      ├─ EFER |= LME         # 开"长模式使能"
      ├─ lgdt gdt64_ptr      # 换一张带 64 位段的 GDT
      ├─ CR0 |= PG           # 开分页——这一拍长模式才真正生效
      └─ ljmp $0x18, $long_mode_entry   # 远跳到 64 位代码段
           └─▶ long_mode_entry (.code64)
                ├─ 数据段 = 0x20、rsp = 0x90000
                ├─ outb 'L', $0xE9       # debugcon 打 'L'
                └─ hlt 循环
```

完成后,`build/debug.log` 里会依次出现 `P`(002 进 PM 时打的)和 `L`(本章进长模式时打的)——连起来就是 `PL`。看到 `L`,就证明 Cinux 已经是货真价实的 64 位 CPU 模式了。

## 为什么现在需要它

你可能觉得 32 位 PM 已经够用了,为什么要费劲进 64 位?因为后面我们要写的是一个真正的 x86_64 内核:它要用 64 位寄存器、要寻址远超 4GB 的内存、要用 `syscall`/`sysret` 这套 64 位专属的快速系统调用。这些在 32 位 PM 里统统做不到。

但进长模式有个硬门槛:**长模式强制要求分页开启**。和 32 位 PM 不同(PM 下分页是可选的),长模式必须建立在"分页已开 + PAE 已开 + 四级页表"的基础上。原因在于,长模式本质上是"在 PAE 四级页表之上加了一层"——CPU 一旦进入长模式,所有地址翻译都得走 PML4→PDPT→PD→PT 这套四级结构,没有分页它根本没法翻译地址。

所以这一章的主线其实就是:**先搭一套刚好够用的临时分页,再按顺序拨开关**。这套分页我们故意做得极简——只恒等映射前 8MB,够 bootloader 自己和接下来要加载的内核跑起来就行。真正的物理内存管理(PMM)和虚拟内存管理(VMM)是 [015](../05-memory/015-mm-pmm.md)/[016](../05-memory/016-mm-vmm.md) 的事,现在不碰。

> 外部依据:Intel SDM Vol.3A §4.1(四级分页)、§4.3(2MB 大页)、§4.5(PAE)、§11.8.2(EFER 与 LME)、§9.8(切换到长模式的固定序列)。

## 设计图

先看这套临时分页长什么样。我们用四级页表里最粗粒度的**2MB 大页**,只填三张表、映射前 8MB:

```text
地址      表            作用
0x1000    PML4          PML4[0] → 指向 PDPT
0x2000    PDPT          PDPT[0] → 指向 PD
0x3000    PD            PD[0..3] → 4 个 2MB 大页,恒等映射 0~8MB
```

为什么三张表就够?因为我们用 2MB 大页,到 PD 这一层就终止了(大页标志位 PS=1 表示"这一项是页,不用再往下查 PT")。一个 PD 项映射 2MB,4 个就盖 8MB。恒等映射的意思是"虚拟地址 = 物理地址"——我们的 bootloader 和内核都在低地址跑,这种最省事的映射刚好够用。

再看进入长模式的**状态机**,顺序是 Intel 定死的,调换一个就三重故障:

```text
32 位 PM
  │  CR3 = 0x1000          # 装载 PML4 基址(让 CPU 知道页表在哪)
  │  CR4 |= PAE (bit 5)    # 开物理地址扩展(长模式的前置条件)
  │  EFER |= LME (bit 8)   # 开"长模式使能"——但此刻还没生效
  │  lgdt gdt64_ptr        # 换上带 64 位代码段的 GDT
  ▼
  CR0 |= PG (bit 31)       # ★ 开分页:LME 此刻"激活",长模式真正生效
  │
  ▼
  ljmp $0x18, $long_mode_entry   # 远跳到 64 位代码段,刷新 CS
  │
  ▼
长模式(64 位)
```

`EFER.LME` 设了之后**并不会立刻生效**——它要等到 `CR0.PG` 被置位的那一拍才真正激活(因为长模式绑死在分页上)。这个"先 LME 后 PG"的顺序是 Intel 的规定,反了就会触发 #GP。

## 代码路线

源码主要在新增的 [long_mode.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/common/long_mode.S)(`setup_page_tables` 和 `enter_long_mode`)以及 [stage2.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/stage2.S) 末尾接上的 `.code64 long_mode_entry` 和扩展 GDT。

### 1. 为什么长模式必须先有分页

(上面"为什么现在需要它"已经讲了原因,这里补一个实操上的关键点。)我们待会儿要 `lgdt`、要远跳、要读内存里的页表本身——这些地址翻译,在分页开启后全部要走我们搭的这套页表。所以**页表必须先搭好、并且正确**,否则 `CR0.PG` 一置位,CPU 连下一条指令的地址都翻译不出来,当场三重故障。这就是为什么 `setup_page_tables` 是第一件事,而且要做成恒等映射:让"搭页表的代码所在的地址"在分页前后都指向同一处,避免"开了分页反而找不到自己"的尴尬。

### 2. setup_page_tables:三张表 + 4 个 2MB 大页

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

### 3. enter_long_mode:顺序即一切

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

### 4. 扩展 GDT:64 位代码段的关键是 L 位

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

### 5. long_mode_entry:64 位段、64 位栈,debugcon 打 'L'

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

## 调试现场

进长模式这一段,坑几乎全在"顺序"和"标志位"上。下面是几个真实调出来的。

**症状一**——`CR0.PG` 一置位,当场三重故障重启。 最高频的原因是页表没搭对:要么某层表没清零(残留垃圾被 CPU 当有效项去查,查到 0 触发缺页),要么 `PD` 的大页项漏了 `Large`(PS)位,CPU 继续往下一层查一个不存在的 PT,当场缺页。定位:在置 `CR0.PG` 那条设断点,`x/4gx 0x3000` 看 `PD[0..3]` 是不是 `0x..83`(带 PS 位)的 2MB 页;`x/1gx 0x1000` 看 `PML4[0]` 是不是 `0x2003`。

**症状二**——置 `EFER.LME` 就崩,或 `CR0.PG` 置位时 #GP。 顺序错了。常见是把 `CR0.PG` 放在 `EFER.LME` 之前(等于在还没"请求"长模式时就开分页),或忘了先开 `CR4.PAE`(长模式的前置)。Intel 对这条序列的检查很严:PAE 没开就置 LME、LME 没置就开 PG,都会 #GP。对着设计图的状态机核一遍顺序。

**症状三**——远跳进 `long_mode_entry` 后又三重故障。 多半是 64 位代码段描述符的 L/D 位错。L=1 时 D 必须为 0,`0x00AF9A000000FFFF` 里的 flags 是 `0xA`(`1010`: G=1,D=0,L=1)——写成 `0xC`(`1100`: D=1,L=0)就是普通的 32 位段,远跳进去 CPU 不认它是长模式,译码错位崩掉。核一遍那个 `.quad` 的字节。

**症状四**——链接时报 64 位重定位错误。 `gdt64_ptr` 用了 `.quad gdt`,而 Stage2 是 32 位 ELF。改成 `.long gdt; .long 0` 就好。这是个纯工具链问题,和 CPU 无关,但挺容易卡住第一次写的人。

## 验证

第一道闸还是构建。老规矩——003 没有运行时自动化测试,构建本身就是冒烟:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -S .
cmake --build build -j$(nproc)
```

`stage2.bin` 里现在嵌了 `.code64` 段,能产出说明汇编器接受了 16/32/64 位混合编码。

第二道闸看 debugcon。`cmake --build build --target run`,跑完看:

```bash
cat build/debug.log    # 期望:PL
```

`P` 是 002 进 PM 时打的、`L` 是本章进长模式时打的。两个都在,说明从实模式一路走到 64 位长模式全程没崩。少了 `P` 或 `L`、或者出现乱码,就照"调试现场"对号入座。

第三道闸用 GDB 确认模式。`cmake --build build --target run-debug`,另一终端:

```text
(gdb) file build/boot/stage2
(gdb) target remote :1234
(gdb) b *long_mode_entry
(gdb) c
# 命中断点 = 远跳成功
(gdb) p/x $cs                         # 应是 0x18(64 位代码段)
(gdb) monitor info registers           # 或看 EFER.LMA 位、CR0.PG 位
```

能停在 `long_mode_entry`、`cs=0x18`、EFER 的 LMA(Long Mode Active)位为 1,就是实打实的 64 位。

## 下一站

现在 Cinux 是一个货真价实的 64 位长模式环境了:有分页、有 64 位寄存器、有一个能跑的栈。可它还停在 bootloader 里 `hlt`——我们还没真正"启动一个内核"。长模式只是把舞台搭好,真正的主角(C++ 写的内核)还没登场。

下一章 [004 · 加载 mini kernel](004-boot-load-mini-kernel.md),我们要让 bootloader 把第一个用 C++ 写的内核镜像从磁盘读进来,跳进它的入口,让真正的"内核代码"第一次跑起来。从那以后,汇编 bootloader 的使命就基本完成,接力棒交给 C++。

---

### 参考

- Intel SDM Vol.3A — §4.1 四级分页(PML4/PDPT/PD/PT 结构)、§4.3 2MB/4MB 大页(PS 位)、§4.5 PAE、§11.8.2 EFER 与长模式使能(LME/MSR `0xC0000080`)、§9.8.1.1 切换到长模式的固定序列(`CR3`→`CR4.PAE`→`EFER.LME`→`CR0.PG`→远跳)。
- OSDev — [Setting Up Long Mode](https://wiki.osdev.org/Setting_Up_Long_Mode)(进入序列与临时恒等映射)、[Page Tables](https://wiki.osdev.org/Page_Table)(四级结构与页表项标志位)、[Creating a 64-bit kernel](https://wiki.osdev.org/Creating_a_64-bit_kernel)(64 位 GDT 的 L 位要求)。
- 本 tag 源码:[long_mode.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/common/long_mode.S)(`setup_page_tables`、`enter_long_mode`)、[stage2.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/stage2.S)(`.code64 long_mode_entry`、扩展 5 项 GDT + `gdt64_ptr`)、[CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/CMakeLists.txt)(`boot_longmode` 对象库)。
- 调试素材提炼自 [1.md](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/document/notes/003/1.md)。

> Intel SDM 版本说明:本卷引用的 SDM 章节号沿用较早版本编号。若按项目本地 PDF(`document/reference/intel/`,2023-06 版)查阅,部分内容已重排——四级分页在 §4.5、2MB 大页见 §4.5、PAE 在 §4.4、EFER 在 §2.2.1、切换到长模式在 Chapter 10。以章节标题为准,别拘泥于编号。

> 参考 URL 的有效性会在全局审查阶段用 open-websearch(bing)统一核活。
