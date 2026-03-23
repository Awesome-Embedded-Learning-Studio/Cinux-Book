# 001_boot_real_mode —— 从 BIOS 到屏幕的第一行字

## 前言 / 动机段

说实话，从零开始写一个操作系统这事儿，我琢磨了好几年。每次看到那些所谓的「操作系统教程」一上来就直接 GRUB 跳转，我心里总觉得少点什么。 BIOS 把 MBR 加载到 0x7C00 这一段，就像把你扔到一个只有 512 字节的荒岛上，周围是 x86 这个几十年的遗产堆。这一章，我们就是要从这荒岛求生开始 —— 让 QEMU 的屏幕左上角依次出现 `Cinux Booting...` 和 `Stage2 OK`。

完成本章后，你能看到一个完整的实模式引导流程：MBR 加载 Stage2，Stage2 开启 A20 地址线，获取 VESA 显卡信息，最后设置 1024x768x32 的线性帧缓冲模式。这一切都发生在内核代码运行之前，是完全裸机的世界。

## 环境说明段

本章的运行环境如下：

- **平台**：x86_64，但运行在 16 位实模式
- **工具链**：GNU AS（AT&T 语法），不用 NASM
- **虚拟机**：QEMU 系统 6.2+（推荐 7.0+）
- **验证方式**：QEMU 图形窗口，通过 BIOS INT 0x10 屏幕输出

## 概念精讲

### 什么是 MBR？

MBR（Master Boot Record）是磁盘的第一个扇区（512 字节）。当你按下电源键，BIOS 做完自检后，会把启动盘的第一个扇区读到内存地址 `0x7C00`，然后跳转过去执行。这时候，CPU 处于**实模式**（Real Mode）—— 16 位寻址，只能访问 1MB 内存，没有保护机制。

你可以把 MBR 理解为 BIOS 交给你的接力棒，一个只有 512 字节的舞台。你在这 512 字节里要做的事情包括：初始化段寄存器、保存启动盘号、在屏幕上打印点什么、然后把第二阶段引导程序（Stage2）从磁盘读进来。

```
BIOS 启动链条：
Power On → POST → 读 MBR 到 0x7C00 → 跳转执行
                                     ↓
                              你的代码接手
                                     ↓
                              加载 Stage2
```

### 为什么需要 Stage2？

MBR 只有 512 字节，还得减去末尾的签名（2 字节），实际可用空间更小。这点空间塞个字符串输出和磁盘读取就差不多了。我们需要一个更大的舞台来准备进入保护模式 —— 这就是 Stage2。

Stage2 被加载到 `0x8000`，有 7.5KB 的空间（15 个扇区），足够我们做更复杂的初始化：开启 A20、获取 VESA 信息、设置视频模式等等。

### DAP 是什么？

DAP（Disk Address Packet）是 BIOS 扩展磁盘读取用的数据结构。传统的 `INT 0x13 AH=0x02` 只能读写 8GB 以下的磁盘（CHB 寻址），现代磁盘动辄几百 GB，必须用 LBA（Logical Block Addressing）方式。

DAP 结构是这样的：

```
Offset  Size    含义
0x00    1 byte  结构大小（固定 0x10）
0x02    1 byte  要读取的扇区数
0x04    2 byte  目标缓冲区偏移
0x06    2 byte  目标缓冲区段
0x08    8 byte  起始 LBA（64 位，但低 32 位够用）
```

### AT&T 汇编语法速查

GNU AS 使用 AT&T 语法，和 Intel 语法有几个关键区别：

| 特性 | AT&T | Intel |
|------|------|-------|
| 操作数顺序 | `src, dest` | `dest, src` |
| 寄存器前缀 | `%eax` | `eax` |
| 立即数前缀 | `$0x10` | `0x10` |
| 内存取值 | `(%eax)` | `[eax]` |
| 字节操作 | `movb` | `mov byte` |
| 字操作 | `movw` | `mov word` |
| 长字操作 | `movl` | `mov dword` |

```
// AT&T 示例
movb $0x10, %al    # 把立即数 0x10 写入 AL
movw %ax, %ds      # 把 AX 写入 DS
lodsb              # 从 DS:SI 读一个字节到 AL，SI 加 1
```

### VESA 和线性帧缓冲

VESA BIOS Extensions（VBE）是 BIOS 提供的一组接口，用来设置显卡模式。我们用的是模式 `0x118`（1024x768x32），关键是启用**线性帧缓冲**（Linear Framebuffer）。

线性帧缓冲的意思是：显存被映射到一段连续的物理地址，你直接往这个地址写像素数据，屏幕就会显示对应颜色。不用像 VGA 文本模式那样还要操作端口、设置光标位置。

```
┌─────────────────────────────────────┐
│ 显存布局（线性帧缓冲）                │
├─────────────────────────────────────┤
│ 0xFD000000 + 0x0000 → (0, 0) 像素   │
│ 0xFD000000 + 0x0004 → (1, 0) 像素   │
│ ...                                  │
│ 0xFD000000 + 0x000C → (3, 0) 像素   │ ← 第一行前 4 个像素
│ ...                                  │
└─────────────────────────────────────┘

每个像素 4 字节（ARGB），pitch（一行字节数）= 4096
```

### A20 地址线

在早期的 8086 CPU 中，地址线只有 20 根，能寻址 1MB 内存。当程序访问超过 1MB 的地址时，地址会回绕到 0（比如 `0xFFFF0 + 0x10 = 0x00000`）。这在当时是个 bug（或者说 feature），但后来 286 及以后的 CPU 有更多地址线，为了兼容老程序，默认情况下第 21 根地址线（A20）是被禁用的。

我们需要显式开启 A20 才能访问超过 1MB 的内存，这对后续进入保护模式很重要。

## 从 0 开始 —— 动手实现

### Step 1：创建 MBR 骨架

**目标**：建立一个能被 BIOS 正确识别的 MBR 文件结构。

**代码**（文件路径：`boot/mbr.S`）：

```asm
/**
 * @file boot/mbr.S
 * @brief Cinux MBR (Master Boot Record) - Real Mode Bootloader
 */

// ============================================================
// 常量和内存布局
// ============================================================
.set STAGE2_LBA,          1       // Stage2 的起始 LBA（紧接 MBR）
.set STAGE2_SECTORS,      15      // Stage2 最大扇区数（7.5KB）
.set STAGE2_LOAD_ADDR,    0x8000  // Stage2 加载地址（0x0000:0x8000）

.section .text
.code16                      // 生成 16 位代码
.global _start                // 让 _start 对链接器可见

// ============================================================
// MBR 入口点
// ============================================================
_start:
    // BIOS 把 MBR 加载到 0x7C00，但 CS 可能不是 0
    // 用远跳转规范化 CS
    ljmp $0, $real_start      # [→CS:IP] 跳转到 0x0000:real_start

real_start:
    // 清零段寄存器（CS 已经被 ljmp 设置好了）
    xorw %ax, %ax             # [→ax] AX = 0
    movw %ax, %ds             # [→ds] DS = 0
    movw %ax, %es             # [→es] ES = 0
    movw %ax, %ss             # [→ss] SS = 0
    movw %ax, %fs             # [→fs] FS = 0
    movw %ax, %gs             # [→gs] GS = 0

    // 设置栈：从 0x7C00 向下增长
    movw $0x7C00, %sp         # [→sp] 栈指针

    // 保存启动盘号（BIOS 把它放在 DL 里）
    movb %dl, boot_drive      # [boot_drive←dl] 保存到变量

    // 打印启动消息
    movw $(msg_booting), %si  # [→si] 消息地址
    call print_string          # [→screen] 调用打印函数

    // 加载 Stage2
    call load_stage2           # [→memory] 从磁盘读取

    // 跳转到 Stage2（永不说再见）
    ljmp $0, $STAGE2_LOAD_ADDR # [→CS:IP] 跳到 0x0000:0x8000

    // 永远不该到这里
    cli                        # [→flags] 禁用中断
    hlt                        # [→cpu] 停机
    jmp _start                 # 死循环

// ============================================================
// 数据段
// ============================================================
.section .data

msg_booting:
    .asciz "Cinux Booting...\n"

boot_drive:
    .byte 0

// ============================================================
// 填充到 510 字节，然后是签名
// ============================================================
.fill 510 - (. - _start), 1, 0
.word 0xAA55                 # 小端序，磁盘上是 55 AA
```

**解释**：

这里有几个关键点。首先，`ljmp $0, $real_start` 这个远跳转很关键 —— BIOS 加载 MBR 时 CS 可能不是 0，这个跳转会同时设置 CS 和 IP，确保 CS=0。其次，我们把所有数据段寄存器都清零，这是实模式的惯用操作，因为内存地址是通过 `段×16 + 偏移` 计算的，段为 0 时地址就是纯偏移。

MBR 签名 `0xAA55` 必须在偏移 510 处，小端序存储，所以 `.word 0xAA55` 会被写成 `55 AA`。BIOS 检查这个签名，没有就不认。

**验证**：暂时无法验证，因为 `print_string` 和 `load_stage2` 还没实现。

### Step 2：实现 print_string 函数

**目标**：用 BIOS INT 0x10 在屏幕上打印字符串。

**代码**（文件路径：`boot/common/serial.S`）：

```asm
/**
 * @file boot/common/serial.S
 * @brief 通用引导函数
 */

.section .text
.global print_string
.global panic

// ============================================================
// print_string - 打印 null 结尾的字符串
// 输入：%si = 字符串地址
// 修改：%ax, %bx, %si
// ============================================================
print_string:
    lodsb                      # [ds:si→al] 读第一个字符
    cmpb $0, %al               # [→flags] 检查是否为 \0
    je .print_done             # [→.print_done] 结束符则退出

.print_loop:
    movb $0x0E, %ah           # [→ah] BIOS teletype 函数
    movw $0x0001, %bx         # [→bx] 页 0，默认颜色
    int $0x10                  # [→video] 调用视频中断

    lodsb                      # [ds:si→al] 读下一个字符
    cmpb $0, %al               # [→flags] 检查结束符
    jne .print_loop            # [→.print_loop] 继续循环

.print_done:
    ret                        # [stack→ip] 返回

// ============================================================
// panic - 打印错误消息并停机
// 输入：%si = 错误消息地址
// 永不返回
// ============================================================
panic:
    call print_string          # [→screen] 打印错误
    cli                        # [→flags] 禁用中断
.halt_loop:
    hlt                        # [→cpu] 停机
    jmp .halt_loop             # 死循环
```

**解释**：

`lodsb` 是个很方便的指令，它从 `DS:SI` 读取一个字节到 `AL`，然后 `SI` 自动加 1。这正好适合遍历字符串。BIOS `INT 0x10 AH=0x0E` 是「teletype 输出」函数，会在当前光标位置打印 `AL` 中的字符，自动处理 `\n` 换行、`\r` 回车。

⚠️ 注意：实模式下字符串必须在可访问的内存范围内。MBR 自身的 `.data` 段是没问题的。

### Step 3：实现 load_stage2 - DAP 磁盘读取

**目标**：使用 BIOS INT 0x13 AH=0x42 扩展读取加载 Stage2。

**代码**（文件路径：`boot/mbr.S`，添加到 `real_start` 后）：

```asm
// ============================================================
// load_stage2 - 使用扩展读取从磁盘加载 Stage2
// 输入：无（使用 STAGE2_LBA 常量）
// 输出：Stage2 被加载到 STAGE2_LOAD_ADDR
// 修改：%ax, %si, %dl
// ============================================================
load_stage2:
    // 在栈上构造 DAP（0x7B00 附近，在栈下方）
    movw $0x7B00, %si          # [→si] DAP 地址

    // 填充 DAP 结构
    movb $0x10, (%si)          # [→0x7B00] DAP 大小 = 0x10
    movb $STAGE2_SECTORS, 2(%si) # [→0x7B02] 扇区数 = 15
    movw $STAGE2_LOAD_ADDR, 4(%si) # [→0x7B04] 偏移 = 0x8000
    movw $0, 6(%si)            # [→0x7B06] 段 = 0x0000
    movl $STAGE2_LBA, 8(%si)   # [→0x7B08] LBA 低 32 位 = 1
    movl $0, 12(%si)           # [→0x7B0C] LBA 高 32 位 = 0

    // 恢复启动盘号（BL 可能被覆盖）
    movb boot_drive, %dl       # [→dl] 恢复 DL

    // 调用 BIOS INT 0x13 AH=0x42
    movw $0x4200, %ax          # [→ax] AH=0x42, AL=0
    int $0x13                  # [→disk] 扩展读取

    // 检查错误（CF=1 表示失败）
    jc disk_error              # [→disk_error] 失败则跳转

    ret                        # [stack→ip] 成功返回

disk_error:
    movw $(msg_disk_error), %si # [→si] 错误消息
    jmp panic                  # [→never] 永不返回

// 数据段添加：
msg_disk_error:
    .asciz "Disk: Failed to load stage2!\n"
```

**解释**：

DAP 结构必须在内存中，我们把它放在 `0x7B00`，刚好在栈（`0x7C00`）下方。`INT 0x13 AH=0x42` 的调用约定：`DS:SI` 指向 DAP，`DL` 是驱动器号。返回时 Carry Flag（CF）置位表示失败，我们用 `jc`（jump if carry）检查。

⚠️ 注意：DAP 的大小字段（偏移 0）必须是 `0x10`，不能少。LBA 从 1 开始（0 是 MBR）。

### Step 4：创建 Stage2 入口

**目标**：Stage2 被加载后打印 "Stage2 OK"。

**代码**（文件路径：`boot/stage2.S`）：

```asm
/**
 * @file boot/stage2.S
 * @brief Stage2 Bootloader - VESA 初始化
 */

.section .text
.code16
.global _start

// 外部函数
.extern print_string
.extern panic
.extern enable_a20
.extern vesa_get_controller_info
.extern vesa_get_mode_info
.extern vesa_set_mode
.extern vesa_save_framebuffer_info

// ============================================================
// Stage2 入口点
// ============================================================
_start:
    // 清零段寄存器
    xorw %ax, %ax             # [→ax] AX = 0
    movw %ax, %ds             # [→ds] DS = 0
    movw %ax, %es             # [→es] ES = 0
    movw %ax, %ss             # [→ss] SS = 0

    // 设置栈（Stage2 从 0x8000 开始，栈设在 0x8000）
    movw $0x8000, %sp         # [→sp] 栈指针

    // 打印 Stage2 OK 消息
    movw $(msg_stage2_ok), %si # [→si] 消息地址
    call print_string          # [→screen] 打印

    // TODO: 后续 VESA 操作...

    // 停机（临时）
    cli                        # [→flags] 禁用中断
.halt_loop:
    hlt                        # [→cpu] 停机
    jmp .halt_loop

.section .data
msg_stage2_ok:
    .asciz "Stage2 OK\n"
```

**解释**：

Stage2 被 MBR 跳转过来时，我们已经知道 CS=0，但其他段寄存器可能不干净，所以重新清零一次是稳妥的。栈设在 `0x8000`，向下增长，不会覆盖 Stage2 代码（代码从 0x8000 开始，栈从 0x8000 向下写）。

### Step 5：开启 A20 地址线

**目标**：使用 BIOS INT 0x15 开启 A20。

**代码**（文件路径：`boot/common/serial.S`）：

```asm
// ============================================================
// enable_a20 - 开启 A20 地址线
// 输出：%al = 1 成功，0 失败
// 修改：%ax, %bx
// ============================================================
.global enable_a20
enable_a20:
    // 尝试 BIOS INT 0x15 AX=0x2401
    movw $0x2401, %ax         # [→ax] A20 开启函数
    int $0x15                  # [→bios] 调用 BIOS
    jc .a20_failed             # [→.a20_failed] CF=1 则失败

    // 检查 AH（返回码）
    cmpb $0, %ah               # [ah→flags] AH=0 表示成功
    jne .a20_failed            # [→.a20_failed] 非零则失败

    // 成功
    movb $1, %al               # [→al] 返回 1
    ret                        # [stack→ip] 返回

.a20_failed:
    // 失败则 panic
    movw $(a20_error_msg), %si # [→si] 错误消息
    jmp panic                  # [→never] 永不返回

.section .data
a20_error_msg:
    .asciz "A20: Failed to enable!\n"
```

**在 stage2.S 中调用**：

```asm
_start:
    ...（段初始化）

    call print_string          # 打印 "Stage2 OK"

    call enable_a20            # [→memory] 开启 A20
    cmpb $1, %al               # [→flags] 检查返回值
    jne .halt                  # [→.halt] 失败则停机
```

**解释**：

A20 开启有几种方法：BIOS 中断、键盘控制器、快速 A20 门。BIOS 方法最简单，`INT 0x15 AX=0x2401` 在现代 BIOS 上基本都支持。返回时 AH=0 表示成功，AL 包含 A20 门状态（1=已开启，0=已关闭）。

### Step 6：获取 VESA 控制器信息

**目标**：调用 INT 0x10 AX=0x4F00 获取 VBE 控制器信息。

**代码**（文件路径：`boot/common/serial.S`）：

```asm
// ============================================================
// VBE 常量
// ============================================================
.set VBE_VBE_INFO_BLOCK,    0x6000  // VbeInfoBlock 缓冲地址
.set VBE_MODE_INFO_BLOCK,   0x6200  // ModeInfoBlock 缓冲地址
.set VBE_FB_INFO_BLOCK,     0x6400  // Framebuffer info 输出地址

// ModeInfoBlock 偏移
.set MODE_PHYS_BASE_PTR,    0x28    // PhysBasePtr (4 bytes)
.set MODE_BYTES_PER_SCAN_LINE, 0x10  // BytesPerScanLine (2 bytes)
.set MODE_X_RESOLUTION,     0x12    // XResolution (2 bytes)
.set MODE_Y_RESOLUTION,     0x14    // YResolution (2 bytes)
.set MODE_BITS_PER_PIXEL,   0x19    // BitsPerPixel (1 byte)

// 目标模式：0x118 | bit14（线性帧缓冲）
.set VESA_TARGET_MODE,      0x4118

// ============================================================
// vesa_get_controller_info - 获取 VBE 控制器信息
// 输出：%al = 0x4F 成功
// 修改：%ax, %bx, %es, %di
// ============================================================
.global vesa_get_controller_info
vesa_get_controller_info:
    // 设置 ES:DI 指向 VbeInfoBlock 缓冲（0x6000）
    movw $0x6000 >> 4, %ax   # [→ax] 段 = 0x6000 / 16 = 0x600
    movw %ax, %es             # [→es] ES = 0x600
    movw $0x0000, %di         # [→di] DI = 0（ES:DI = 0x6000:0 = 0x6000）

    // 设置 VBEInfoBlock 签名为 "VBE2"（启用 VBE 2.0+ 功能）
    movl $0x32454256, %es:(0) # [→es:0] 写入 "VBE2"

    // 调用 INT 0x10 AX=0x4F00
    movw $0x4F00, %ax         # [→ax] VBE 获取控制器信息
    int $0x10                  # [→bios] 调用视频中断

    // 检查返回值（AL 应该是 0x4F）
    cmpb $0x4F, %al            # [al→flags]
    jne .vesa_ctrl_failed      # [→.vesa_ctrl_failed] 不支持

    // 检查返回码（AH 应该是 0）
    cmpb $0, %ah               # [ah→flags]
    jne .vesa_ctrl_failed      # [→.vesa_ctrl_failed] 失败

    ret                        # [stack→ip] 成功返回

.vesa_ctrl_failed:
    movw $(vesa_ctrl_error_msg), %si # [→si] 错误消息
    jmp panic

.section .data
vesa_ctrl_error_msg:
    .asciz "VESA: Controller info failed!\n"
```

**解释**：

VbeInfoBlock 是一个 512 字节的结构，BIOS 会把显卡信息填进去。我们把它放在 `0x6000`（段 `0x600`，偏移 `0`）。设置签名为 `"VBE2"` 告诉 BIOS 我们想要 VBE 2.0 或更高版本的信息。返回时 `AL=0x4F` 表示支持 VBE，`AH=0` 表示调用成功。

### Step 7：获取模式信息

**目标**：获取 0x118 模式的详细信息。

**代码**（文件路径：`boot/common/serial.S`）：

```asm
// ============================================================
// vesa_get_mode_info - 获取 0x118 模式信息
// 输出：%al = 0x4F 成功
// 修改：%ax, %bx, %cx, %es, %di
// ============================================================
.global vesa_get_mode_info
vesa_get_mode_info:
    // 设置 ES:DI 指向 ModeInfoBlock 缓冲（0x6200）
    movw $0x6200 >> 4, %ax   # [→ax] 段 = 0x620
    movw %ax, %es             # [→es] ES = 0x620
    movw $0x0000, %di         # [→di] DI = 0

    // 调用 INT 0x10 AX=0x4F01 CX=0x0118
    movw $0x4F01, %ax         # [→ax] VBE 获取模式信息
    movw $0x0118, %cx         # [→cx] 模式号 0x118
    int $0x10                  # [→bios] 调用视频中断

    // 检查返回值
    cmpb $0x4F, %al            # [al→flags]
    jne .vesa_mode_failed      # [→.vesa_mode_failed]

    cmpb $0, %ah               # [ah→flags]
    jne .vesa_mode_failed      # [→.vesa_mode_failed]

    ret                        # [stack→ip] 成功

.vesa_mode_failed:
    movw $(vesa_mode_error_msg), %si # [→si] 错误消息
    jmp panic

.section .data
vesa_mode_error_msg:
    .asciz "VESA: Mode 0x118 not available!\n"
```

**解释**：

ModeInfoBlock 是一个 256 字节的结构，放在 `0x6200`。我们最关心的是 `PhysBasePtr`（帧缓冲物理地址）、`BytesPerScanLine`（pitch）、`XResolution/YResolution`（分辨率）、`BitsPerPixel`（色深）。这些信息在下一步设置模式后会被提取出来保存。

### Step 8：设置视频模式

**目标**：设置 0x118 模式并启用线性帧缓冲。

**代码**（文件路径：`boot/common/serial.S`）：

```asm
// ============================================================
// vesa_set_mode - 设置 VESA 视频模式
// 输出：%al = 0x4F 成功
// 修改：%ax, %bx
// ============================================================
.global vesa_set_mode
vesa_set_mode:
    // 调用 INT 0x10 AX=0x4F02 BX=0x4118
    // bit 14 (0x4000) 启用线性帧缓冲
    movw $0x4F02, %ax         # [→ax] VBE 设置模式
    movw $VESA_TARGET_MODE, %bx # [→bx] 0x4118
    int $0x10                  # [→bios] 调用视频中断

    // 检查返回值
    cmpb $0x4F, %al            # [al→flags]
    jne .vesa_set_failed       # [→.vesa_set_failed]

    cmpb $0, %ah               # [ah→flags]
    jne .vesa_set_failed       # [→.vesa_set_failed]

    ret                        # [stack→ip] 成功

.vesa_set_failed:
    movw $(vesa_set_error_msg), %si # [→si] 错误消息
    jmp panic

.section .data
vesa_set_error_msg:
    .asciz "VESA: Failed to set mode!\n"
```

**解释**：

`BX=0x4118` 中的 `bit 14`（`0x4000`）是关键，它启用线性帧缓冲。没有这个位，我们得到的是银行切换模式，需要频繁调用 BIOS 切换显存窗口，非常麻烦。线性模式下，整个帧缓冲是一段连续的物理地址，直接读写就行。

### Step 9：保存帧缓冲信息

**目标**：从 ModeInfoBlock 提取关键信息并保存到 `0x6400`。

**代码**（文件路径：`boot/common/serial.S`）：

```asm
// Framebuffer info 布局（16 字节）
.set FB_INFO_PHYS_ADDR,  0       // 物理地址 (8 bytes)
.set FB_INFO_PITCH,      8       // 每行字节数 (4 bytes)
.set FB_INFO_WIDTH,      12      // 宽度 (2 bytes)
.set FB_INFO_HEIGHT,     14      // 高度 (2 bytes)

// ============================================================
// vesa_save_framebuffer_info - 保存帧缓冲信息到 0x6400
// 修改：%ax, %bx, %es, %di, %gs
// ============================================================
.global vesa_save_framebuffer_info
vesa_save_framebuffer_info:
    // 源：ES:DI = 0x6200 (ModeInfoBlock)
    // 目标：GS = 0x640 (0x6400 的段)
    movw $0x6200 >> 4, %ax   # [→ax] 源段 = 0x620
    movw %ax, %es             # [→es] ES = 0x620

    movw $0x6400 >> 4, %bx   # [→bx] 目标段 = 0x640
    movw %bx, %gs             # [→gs] GS = 0x640
    movw $0x0000, %di         # [→di] DI = 0

    // 读取 PhysBasePtr (offset 0x28, 4 bytes) 写入 0x6400+0
    movl MODE_PHYS_BASE_PTR(%di), %eax # [es:0x28→eax]
    movl %eax, FB_INFO_PHYS_ADDR(%di)  # [eax→gs:0x6400+0]

    // 高 32 位清零（PhysBasePtr 实际是 4 字节）
    movl $0, FB_INFO_PHYS_ADDR+4(%di)  # [0→gs:0x6400+4]

    // 读取 BytesPerScanLine (offset 0x10, 2 bytes) 写入 0x6400+8
    movw MODE_BYTES_PER_SCAN_LINE(%di), %ax # [es:0x10→ax]
    movw %ax, FB_INFO_PITCH(%di)         # [ax→gs:0x6400+8]

    // 读取 XResolution (offset 0x12, 2 bytes) 写入 0x6400+12
    movw MODE_X_RESOLUTION(%di), %ax     # [es:0x12→ax]
    movw %ax, FB_INFO_WIDTH(%di)         # [ax→gs:0x6400+12]

    // 读取 YResolution (offset 0x14, 2 bytes) 写入 0x6400+14
    movw MODE_Y_RESOLUTION(%di), %ax     # [es:0x14→ax]
    movw %ax, FB_INFO_HEIGHT(%di)        # [ax→gs:0x6400+14]

    ret                        # [stack→ip] 完成
```

**在 stage2.S 中调用**：

```asm
_start:
    ...（前面的初始化）

    call enable_a20            # 开启 A20
    cmpb $1, %al               # 检查返回值
    jne .halt                  # 失败则停机

    call vesa_get_controller_info # 获取 VBE 信息
    call vesa_get_mode_info    # 获取 0x118 模式信息
    call vesa_set_mode         # 设置视频模式
    call vesa_save_framebuffer_info # 保存帧缓冲信息

    movw $(msg_vesa_ok), %si   # 成功消息
    call print_string

    cli
.halt_loop:
    hlt
    jmp .halt_loop
```

**解释**：

我们把帧缓冲信息打包成 16 字节的结构放在 `0x6400`，这样后续的内核代码可以很容易找到这些信息。QEMU 典型的返回值是：`PhysBasePtr=0xFD000000`，`pitch=4096`，`1024x768`，`32bpp`。

### Step 10：构建脚本更新

**目标**：创建构建脚本，把 MBR 和 Stage2 写入磁盘镜像。

**代码**（文件路径：`scripts/build_image.sh`）：

```bash
#!/bin/bash
#
# scripts/build_image.sh
# @brief 构建 Cinux 磁盘镜像
#

set -e  # 遇到错误立即退出

# ============================================================
# 路径配置
# ============================================================
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(dirname "$SCRIPT_DIR")
BUILD_DIR=${PROJECT_ROOT}/build

# 解析命令行参数
MBR_BIN=${1:-${BUILD_DIR}/boot/mbr.bin}
STAGE2_BIN=${2:-${BUILD_DIR}/boot/stage2.bin}
OUTPUT_IMAGE=${3:-${BUILD_DIR}/cinux.img}

# 确保构建目录存在
mkdir -p "$BUILD_DIR"

# ============================================================
# 验证输入文件
# ============================================================
if [ ! -f "$MBR_BIN" ]; then
    echo "错误: MBR 文件不存在: $MBR_BIN"
    echo "请先运行 'make' 构建引导程序"
    exit 1
fi

if [ ! -f "$STAGE2_BIN" ]; then
    echo "错误: Stage2 文件不存在: $STAGE2_BIN"
    echo "请先运行 'make' 构建引导程序"
    exit 1
fi

# ============================================================
# 常量
# ============================================================
STAGE2_LBA=1                # Stage2 起始 LBA
STAGE2_MAX_SECTORS=15       # Stage2 最大扇区数

# 获取 Stage2 实际大小
STAGE2_SIZE=$(stat -c%s "$STAGE2_BIN" 2>/dev/null || stat -f%z "$STAGE2_BIN")
STAGE2_SECTORS=$(( (STAGE2_SIZE + 511) / 512 ))

# 验证 Stage2 大小
if [ $STAGE2_SECTORS -gt $STAGE2_MAX_SECTORS ]; then
    echo "错误: Stage2 过大: $STAGE2_SIZE 字节 ($STAGE2_SECTORS 扇区)"
    echo "       最大允许: $STAGE2_MAX_SECTORS 扇区"
    exit 1
fi

# ============================================================
# 创建磁盘镜像
# ============================================================
# 创建 1MB 空白镜像
dd if=/dev/zero of="$OUTPUT_IMAGE" bs=1M count=1 status=none

# 写入 MBR 到扇区 0
dd if="$MBR_BIN" of="$OUTPUT_IMAGE" bs=512 count=1 conv=notrunc status=none
echo "MBR 写入到扇区 0"

# 写入 Stage2 到扇区 1
dd if="$STAGE2_BIN" of="$OUTPUT_IMAGE" bs=512 seek=$STAGE2_LBA conv=notrunc status=none
echo "Stage2 写入到扇区 $STAGE2_LBA-$((STAGE2_LBA + STAGE2_SECTORS - 1))"

# ============================================================
# 验证镜像
# ============================================================
# 检查 MBR 签名
SIGNATURE=$(dd if="$OUTPUT_IMAGE" bs=1 skip=510 count=2 status=none | xxd -p)
if [ "$SIGNATURE" = "55aa" ]; then
    echo "MBR 签名有效: 0xAA55"
else
    echo "警告: MBR 签名无效: $SIGNATURE"
fi

echo ""
echo "磁盘镜像构建成功!"
echo "  路径: $OUTPUT_IMAGE"
echo ""
echo "运行 Cinux:"
echo "  make run"
```

**解释**：

这个脚本创建一个 1MB 的磁盘镜像，把 MBR 放在扇区 0，Stage2 放在扇区 1-15。`conv=notrunc` 很重要，它告诉 `dd` 不要截断输出文件 —— 我们希望保持 1MB 的镜像大小。最后脚本验证 MBR 签名是否正确。

### Step 11：CMakeLists.txt 配置

**目标**：配置构建系统。

**代码**（文件路径：`boot/CMakeLists.txt`）：

```cmake
# Bootloader CMakeLists.txt

set(BOOT_SOURCES
    common/serial.S
    mbr.S
)

set(STAGE2_SOURCES
    common/serial.S
    stage2.S
)

# MBR
add_executable(mbr.elf ${BOOT_SOURCES})
target_link_options(mbr.elf PRIVATE "-T${CMAKE_SOURCE_DIR}/boot/mbr.ld")
set_target_properties(mbr.elf PROPERTIES OUTPUT_NAME "mbr")
set_target_properties(mbr.elf PROPERTIES SUFFIX "")

# 转换为纯二进制
add_custom_command(
    OUTPUT mbr.bin
    DEPENDS mbr.elf
    COMMAND ${CMAKE_OBJCOPY} -O binary mbr.elf mbr.bin
    VERBATIM
)

add_custom_target(mbr_bin ALL DEPENDS mbr.bin)

# Stage2
add_executable(stage2.elf ${STAGE2_SOURCES})
target_link_options(stage2.elf PRIVATE "-T${CMAKE_SOURCE_DIR}/boot/stage2.ld")
set_target_properties(stage2.elf PROPERTIES OUTPUT_NAME "stage2")
set_target_properties(stage2.elf PROPERTIES SUFFIX "")

# 转换为纯二进制
add_custom_command(
    OUTPUT stage2.bin
    DEPENDS stage2.elf
    COMMAND ${CMAKE_OBJCOPY} -O binary stage2.elf stage2.bin
    VERBATIM
)

add_custom_target(stage2_bin ALL DEPENDS stage2.bin)

# 构建镜像
add_custom_target(
    image
    DEPENDS mbr_bin stage2_bin
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/build_image.sh
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    VERBATIM
)

# 运行目标
add_custom_target(
    run
    DEPENDS image
    COMMAND qemu-system-x86_64
        -drive file=${CMAKE_BINARY_DIR}/cinux.img,format=raw
        -serial stdio
        -nographic
    VERBATIM
)
```

**链接脚本**（文件路径：`boot/mbr.ld`）：

```ld
/* MBR 链接脚本 - 代码从 0x7C00 开始 */
OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)

SECTIONS {
    . = 0x7C00;

    .text : {
        *(.text)
    }

    .data : {
        *(.data)
    }

    /* 确保 MBR 不超过 512 字节 */
    _mbr_end = .;
    ASSERT(_mbr_end <= 0x7E00, "MBR too large!")
}
```

**链接脚本**（文件路径：`boot/stage2.ld`）：

```ld
/* Stage2 链接脚本 - 代码从 0x8000 开始 */
OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)

SECTIONS {
    . = 0x8000;

    .text : {
        *(.text)
    }

    .data : {
        *(.data)
    }

    /* 确保 Stage2 不超过 15 扇区 */
    _stage2_end = .;
    ASSERT(_stage2_end <= 0x9C00, "Stage2 too large!")
}
```

**解释**：

链接脚本告诉链接器代码应该从哪个地址开始。MBR 必须从 `0x7C00` 开始（BIOS 加载地址），Stage2 从 `0x8000` 开始（我们选择的加载地址）。`ASSERT` 确保我们没有超出大小限制。

## 构建与运行

现在我们来编译运行，看看到底能不能成功。从当前 tag checkout 开始：

```bash
# 进入项目目录
cd /path/to/cinux

# 创建构建目录
mkdir build
cd build

# 配置 CMake
cmake -DCMAKE_BUILD_TYPE=Debug -B . -S ..

# 编译
make

# 运行
make run
```

或者直接用 QEMU：

```bash
qemu-system-x86_64 -drive file=build/cinux.img,format=raw
```

你应该看到 QEMU 窗口左上角依次出现：

```
Cinux Booting...
Stage2 OK
```

如果看到这些字，恭喜！你已经成功完成了从 BIOS 到实模式引导的完整链条。

## 调试技巧

说实话，这一章踩坑最多的地方就是屏幕上一片漆黑，什么都没有。这里给你几个调试方法：

### 1. 检查 MBR 签名

```bash
xxd build/cinux.img | tail -2
# 应该看到：
# 00001f0: 0000 0000 0000 0000 0000 0000 0000 55aa  ......U.
```

最后四个字节必须是 `55 aa`（小端序）。

### 2. 检查 Stage2 是否正确写入

```bash
xxd build/cinux.img | head -5
# 扇区 0 是 MBR
# 扇区 1 从偏移 0x200 开始，应该是 Stage2 的开头
xxd build/cinux.img | grep "^0000200"
```

### 3. 用 QEMU monitor 调试

启动 QEMU 时加 `-monitor stdio`，然后按 Ctrl+A 然后 C 进入 monitor：

```
(qemu) info registers
(qemu) xp /512x 0x7C00    # 查看 MBR 内存
(qemu) xp /512x 0x8000    # 查看 Stage2 内存
```

### 4. 用 GDB 调试

```bash
qemu-system-x86_64 -drive file=build/cinux.img,format=raw -s -S
```

然后在另一个终端：

```bash
gdb
(gdb) target remote :1234
(gdb) set architecture i8086  # 实模式是 16 位
(gdb) break *0x7C00
(gdb) continue
```

### 常见问题

**问题 1：屏幕没有任何输出**

检查 `print_string` 函数的 `int $0x10` 调用。确认 `AH=0x0E`，`BX` 设置正确。确认字符串以 `\0` 结尾。

**问题 2：看到 "Disk: Failed to load stage2!"**

检查 DAP 结构是否正确填写。检查 `STAGE2_LBA` 是否和 `build_image.sh` 中的 `seek` 值一致。用 `xxd` 确认 Stage2 确实被写入了扇区 1。

**问题 3：VESA 相关错误**

有些 QEMU 版本默认的 VGA 卡可能不支持 VBE 2.0。尝试启动参数加 `-vga std` 或 `-vga cirrus`。

## 本章小结

到这里，我们已经完成了实模式下的全部引导流程。用一张表格总结一下本章新增的关键内容：

| 函数/结构 | 位置 | 功能 |
|-----------|------|------|
| `_start` | `boot/mbr.S` | MBR 入口，初始化并加载 Stage2 |
| `print_string` | `boot/common/serial.S` | BIOS 字符串输出 |
| `load_stage2` | `boot/mbr.S` | DAP 扩展磁盘读取 |
| `enable_a20` | `boot/common/serial.S` | 开启 A20 地址线 |
| `vesa_get_controller_info` | `boot/common/serial.S` | 获取 VBE 信息 |
| `vesa_get_mode_info` | `boot/common/serial.S` | 获取 0x118 模式信息 |
| `vesa_set_mode` | `boot/common/serial.S` | 设置视频模式 |
| `vesa_save_framebuffer_info` | `boot/common/serial.S` | 保存帧缓冲信息到 0x6400 |
| DAP 结构 | `boot/mbr.S` | 磁盘地址包，用于扩展读取 |
| VbeInfoBlock | 0x6000 | VBE 控制器信息缓冲 |
| ModeInfoBlock | 0x6200 | VBE 模式信息缓冲 |
| FramebufferInfo | 0x6400 | 帧缓冲信息输出 |

下一章（`002_boot_gdt_protected`）我们将进入保护模式，设置 GDT，并通过 QEMU debugcon 输出字符 `P` 确认成功。实模式的旅程到此结束，接下来是更刺激的 32 位世界！

---

**文件路径汇总**：

- [boot/mbr.S](../../boot/mbr.S)
- [boot/stage2.S](../../boot/stage2.S)
- [boot/common/serial.S](../../boot/common/serial.S)
- [scripts/build_image.sh](../../scripts/build_image.sh)
- [boot/CMakeLists.txt](../../boot/CMakeLists.txt)
- [boot/mbr.ld](../../boot/mbr.ld)
- [boot/stage2.ld](../../boot/stage2.ld)
