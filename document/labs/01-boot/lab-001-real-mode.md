---
title: Lab 001 · 实模式引导
---

# Lab 001 · 实模式引导

> 这个 lab 配套 [001 · 实模式引导](../../book/01-boot/001-boot-real-mode.md)。目标是亲手写出一个"通电 → 读盘 → 点亮屏幕 → 配好图形模式"的实模式两段式引导。这里只给任务、约束和验证手段,**选择子自己算、DAP 自己拼、段自己理**,不贴现成答案。

## 实验目标

从一张空盘开始,做出一个能被 BIOS 引导的镜像 `cinux.img`:

- **MBR**(扇区 0,512B):理顺段、设栈、用 `INT 0x13 AH=0x42` 把 Stage2 读到 `0x8000`、远跳过去。
- **Stage2**(扇区 1+):开 A20、用 VESA 切到 1024×768 图形模式、把 framebuffer 参数存到 `0x6400`。
- 跑起来能在 QEMU 窗口看到 `Cinux Booting...` → `Stage2 OK` → `Mode info OK, switching...`,然后屏幕切进图形模式。

## 前置条件

- 已完成 `000`:交叉工具链(`as`/`ld`/`objcopy`)、CMake、`qemu-system-x86_64` 就绪,`cmake -B build -S .` 能配置通过。
- 能看懂 AT&T 汇编语法(源操作数在前、`%` 寄存器前缀、`$` 立即数前缀)。

## 任务分解

别想一口气写完,分四块走。

### 第一块:MBR 骨架,先点亮一个字符

写 `boot/mbr.S`,入口 `_start` 放在会被链接到 `0x7C00` 的位置。先做最小闭环:`cli` → 段归一化(`DS=ES=SS=CS`)→ `cld` → 设栈 → `sti` → 打印一两个字符 → 死循环。先用 `INT 0x10 AH=0x0E` 直接往 `al` 塞字符试试水,**单字符能出来**是后面一切的前提。最后别忘 `.org 510` + `.word 0xAA55`——没有这个魔数,BIOS 根本不认这是可引导扇区。

链接脚本要 `. = 0x7C00`。这一块跑通,你就验证了"段理顺了、魔数对了、BIOS 真的把我的代码读进来了"。

### 第二块:DAP + 扩展读,把 Stage2 读进来

实现 `load_stage2`:在 `0x7B00` 拼 16 字节的 DAP(大小、扇区数、目标 `段:偏移=0:0x8000`、起始 LBA=1),`movb boot_drive, %dl` 恢复盘号,`movw $0x4200, %ax` + `int $0x13`,`jc` 判错。读完 `ljmp $0x800, $0` 远跳到 `0x8000`。

注意 `dl` 要在 MBR 一进来就存好(`movb %dl, boot_drive`)——BIOS 后续的中断可能改掉它,不存就再也读不了盘。

这一块跑通的标志:Stage2 里随便写个打印,能在窗口看到——说明磁盘第 1 扇区真的被读进来了。

### 第三块:Stage2 段/栈重置,开 A20

写 `boot/stage2.S`。它链接在 `0x0`、被读到 `0x8000`,所以 `_start` 里要把 `DS=ES=FS=GS=CS=0x800`、`SS=0x900`(栈基址物理 `0x9000`)、`sp=0xFFFE`。想清楚为什么是"链接 `0x0` + 段寄存器承载实际位置",别让自己掉进"双重偏移"的坑。

然后调 `enable_a20`(`movw $0x2401, %ax; int $0x15; jc 失败`)。A20 打不开,后面进保护模式碰高地址就会回绕——趁 BIOS 还在,先打开。

### 第四块:VESA 三步 + 存 framebuffer

写 `boot/common/serial.S`(会被链进 Stage2,但**绝不**链进 MBR):

- `vesa_get_controller_info`:缓冲区 `0x6000` 开头先写 `"VBE2"` 签名,`AX=0x4F00` + `INT 0x10`。
- `vesa_get_mode_info`:`0x6200` 接收,`AX=0x4F01` + `CX=0x0118`。
- `vesa_set_mode`:`AX=0x4F02` + `BX=0x4118`(第 14 位 = 线性 framebuffer)。
- `vesa_save_framebuffer_info`:从 `0x6200` 的 ModeInfo 把物理地址(偏移 `0x28`)、pitch(`0x10`)、宽(`0x12`)、高(`0x14`)抄到 `0x6400`。
- 顺带写个带 `push %ax/%bx/%si` 保护的 `print_string`——记住 BIOS 中断会弄脏寄存器。

每一步都要查 `AL` 是不是 `0x4F`、`AH` 是不是 `0`,不是就 `jmp panic` 打印错误串,别让失败静默过去。

## 接口约束

下面这些得自己保证对、但 lab 不给现成代码,照着核:

- **内存布局常量**:MBR 栈 `0x7000`、DAP `0x7B00`、MBR 代码 `0x7C00`、Stage2 代码 `0x8000`、Stage2 栈基址 `0x9000`、VBE 控制器信息 `0x6000`、模式信息 `0x6200`、framebuffer 存档 `0x6400`。这些地址之间不能打架(尤其栈别压进代码区)。
- **链接地址**:MBR 链接 `0x7C00`;Stage2 链接 `0x0`、运行时段寄存器 = `0x800`。两套不能搞混。
- **512 字节红线**:`mbr.bin` 必须正好 512 字节,末两字节 `0xAA55`。MBR 只能链 `mbr.S`,`common/serial.S` 一行都不能进。
- **魔数**:`0x4118 = 0x118 | (1<<14)`;VBE 成功返回 `AL=0x4F` 且 `AH=0`。
- **磁盘布局**:扇区 0 = MBR,扇区 1+ = Stage2(≤15 扇区 = 7.5KB)。
- **AT&T 语法**:源在前目的在后;`movb %dl, boot_drive` 是"把 `dl` 存进 `boot_drive`",别写反。

## 验证步骤

**第一道闸:能构建。** 001 没有运行时自动化测试,构建本身就是冒烟:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -S .
cmake --build build -j$(nproc)
```

检查 `build/boot/mbr.bin` 是不是 512 字节、`scripts/build_image.sh` 有没有报 `MBR signature valid: 0xAA55`。`mbr.bin` 超过 512 字节,构建阶段就该发现(链接脚本 + `.org 510` 会让多余的代码挤掉魔数或溢出)。

**第二道闸:QEMU 观察。** `cmake --build build --target run`,**看 QEMU 窗口**(不是命令行 stdout——`INT 0x10 AH=0x0E` 走 VGA teletype,显示在窗口里,不在 `-serial stdio`)。按序看到:

```text
Cinux Booting...
Stage2 OK
Mode info OK, switching...
```

然后屏幕切进图形模式变黑、机器停住。这就是通过信号。

**第三道闸:GDB 核 framebuffer 存档。** `cmake --build build --target run-debug` 起带 `-s -S` 的 QEMU,另一终端:

```text
(gdb) target remote :1234
(gdb) x/2gx 0x6400
```

低 8 字节应是合理的显存物理地址(`0xfd000000` 附近一类),后面是 pitch/宽/高。能读到合理地址,说明 VESA 真走通了。

## 常见故障

几个几乎必踩的坑,先给你提个醒。

**单字符能打、字符串打不出/乱码。** 八成是段没理顺——`CS` 归零了但 `DS` 没跟上,`DS:SI` 指向了别处。检查 `_start` 里 `DS=ES=SS=CS` 是不是真的全设了。

**Stage2 跳进去能跑、一访问数据就炸。** 典型"双重偏移":链接地址和运行时段没配合好。记住口诀——要么"绝对链接 + `DS=0`",要么"链接 `0x0` + 段承载位置",别两套混用。

**MBR 里 `call` 一个函数就重启,挪到 Stage2 却没事。** MBR 超 512 字节了,多出来的没被加载。`objdump -d mbr.bin` 或直接看文件大小。MBR 只能链 `mbr.S`。

**打印一两次后莫名飞掉。** `print_string` 没保护寄存器,BIOS 中断污染了 `ax/bx/si`,调用者指针被毁。把 `push/pop` 补上。

**VESA 某步失败、屏幕没切。** 看有没有 `VESA: ... failed!` 的 panic 输出定位是哪一步;常见是请求控制器信息前忘了写 `"VBE2"` 签名,或设模式时漏了线性 framebuffer 那一位(`0x4000`)。

**切进图形模式后"以为成功了"但其实崩了。** 文本一消失就没法看输出了。这种情况靠 GDB 单步过 `vesa_set_mode` 之后的指令、看 `rip` 有没有继续往前走,别只凭"屏幕黑了"就下结论。

## 通过标准

- `cmake --build build` 成功,`mbr.bin` 为 512 字节且魔数 `0xAA55` 正确。
- `make run` 后 QEMU 窗口依次显示 `Cinux Booting...`、`Stage2 OK`、`Mode info OK, switching...`,随后切进图形模式。
- GDB 能在 `0x6400` 读到合理的 framebuffer 物理地址与 pitch/宽/高。
- 全程没有碰保护模式、GDT、CR0——那是 [Lab 002](../01-boot/lab-002-gdt-protected.md) 的事。
