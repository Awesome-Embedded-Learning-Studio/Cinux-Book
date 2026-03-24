# 004_boot_load_mini_kernel_A · 加载小内核（准备篇）

> 本章完成后的可见效果：QEMU 不崩溃，debugcon 输出 `J`（确认即将跳转小内核），随后小内核接管
>
> 前置要求：已完成 `003_boot_long_mode`，理解 Long Mode 基本概念

---

## 一、前言：为什么要分两步加载内核

说实话，如果直接让你从零实现一个完整的 ELF64 加载器，很容易被各种细节淹没——Program Header 解析、段重定位、虚拟地址映射……每一项都够折腾半天。更别提我们还处于"刚进 Long Mode"这个脆弱阶段，一个 `NULL` 指针就能让整个系统 triple fault。

所以这一章我们采取"分治策略"：先把最基础的 E820 内存枚举和磁盘读取搞定，暂不解析 ELF。目标很明确——在 Long Mode 之前完成以下两件事：

1. **E820 内存探测**：搞清楚系统有哪些内存可用，保存到固定位置 `0x5000`
2. **磁盘读取 ELF header**：从 LBA=16 读取内核头部到 `0x10000`，为后续解析做准备

这一步完成后，我们会输出一个 `J` 字符（Jump）表示"准备跳转到小内核"，下一章再真正实现 ELF 解析和跳转。

**这一步之后，我们就能：**
- 知道系统中哪些内存区域可用（type=1）
- 从磁盘正确读取数据到内存
- 为后续完整的 ELF 加载铺路

---

## 二、核心概念精讲

### 2.1 E820 内存探测是什么？

E820 是 BIOS 提供的一个中断接口（INT 0x15, AX=0xE820），用于查询系统内存布局。在 32 位时代，BIOS 会告诉你"哪里有内存、哪里是保留区"。这在启动阶段非常关键，因为：

* 低 1MB 有很多"特殊区域"（BIOS 数据区、视频内存等）
* 系统可能有不连续的内存条
* 有些区域是 ACPI 保留的，不能乱用

你可以把 E820 理解为：BIOS 给你一张"内存地图"，上面标注了哪些地皮可以盖楼（可用内存）、哪些是绿化带（保留区）。

### 2.2 E820 调用流程

E820 的调用方式有点"特别"：你需要反复调用同一个中断，每次 BIOS 返回一条内存记录，然后把 EBX 作为"续接值"传给下一次调用。当 EBX=0 时，表示没有更多记录了。

```
第一次调用：EBX=0 → BIOS 返回第一条记录，EBX≠0
第二次调用：EBX=上次返回值 → BIOS 返回第二条记录，EBX≠0
...
第 N 次调用：EBX=上次返回值 → BIOS 返回最后一条记录，EBX=0
```

每次调用返回的记录格式如下（24 字节）：

```
Offset  Size  Meaning
------  ----  -------
0       8     Base Address（物理地址，64 位）
8       8     Length（长度，64 位）
16      4     Type（类型，32 位）
20      4     ACPI 3.0 扩展字段（可选）
```

Type 字段最关键：
* `1` = 可用内存（RAM）
* `2` = 保留区（ROM、ACPI NVS 等）
* `3` = ACPI Reclaimable
* `4` = NVS
* 其他 = 各种保留类型

⚠️ **注意**：只有 Type=1 的区域才能自由使用！

### 2.3 INT 13h 扩展读取

在 Real Mode 下读取磁盘，传统方法是 CHS（柱面/磁头/扇区）寻址，但这个方法早就过时了。现代 BIOS 提供"扩展读取"功能（INT 0x13, AH=0x42），使用 LBA（逻辑块寻址）直接指定扇区号。

扩展读取需要一个"DAP"（Disk Address Packet）结构：

```
Offset  Size  Meaning
------  ----  -------
0       1     Packet Size（必须为 0x10 = 16）
1       1     Reserved（必须为 0）
2       2     Number of Sectors to Transfer
4       2     Buffer Offset（16 位）
6       2     Buffer Segment（16 位）
8       8     Starting LBA（64 位）
```

这里有个坑：Buffer 是用 `段:偏移` 格式表示的。比如我们想读到物理地址 `0x10000`，需要拆成 `0x1000:0x0000`（因为 `0x1000 << 4 + 0x0000 = 0x10000`）。

### 2.4 为什么要在 Real Mode 做？

你可能会问：我们不是已经进入 Long Mode 了吗？为什么不直接在 64 位模式下读磁盘？

答案很现实：**BIOS 中断只能在 Real Mode 下调用**。一旦进入保护模式或 Long Mode，BIOS 中断就不可用了（除非模拟 V86 模式，但那是另一回事）。所以必须在进入保护模式之前把磁盘读取搞定。

这就是 Stage2 的执行顺序：
1. Real Mode 下调用 E820 和 INT 13h
2. 切换到 Protected Mode
3. 切换到 Long Mode

---

## 三、AT&T 汇编语法速查

本章新增的 AT&T 语法点：

| 操作 | Intel 语法 | AT&T 语法 | 说明 |
|------|-----------|----------|------|
| 段寄存器前缀 | `mov ax, ds:[0]` | `movl %ds:0, %eax` | 显式指定段 |
| 立即数内存写入 | `mov byte [0x5000], 0` | `movb $0, 0x5000` | 注意 `$` 前缀 |
| 16 位寄存器 | `mov ax, 0x5000` | `movw $0x5000, %ax` | `w` 后缀 |
| 字符串操作 | `rep stosd` | `rep stosl` | `l` 表示 long |

**重要提示**：AT&T 语法的操作数顺序是"源, 目标"，和 Intel 语法相反！

---

## 四、动手实现

### Step 1：创建公共引导模块

**目标**：创建 `boot/common/boot.S`，封装 E820 内存枚举和磁盘读取功能

**代码**（文件路径：`boot/common/boot.S`）：

```asm
/**
 * @file boot/common/boot.S
 * @brief Real mode memory enumeration and kernel loading functions
 *
 * This file provides two critical operations that must complete before
 * entering protected mode:
 *   1. E820 memory enumeration: Query system memory layout, save to 0x5000 buffer
 *   2. Disk read kernel: Read kernel ELF from LBA=16 to 0x10000
 *
 * Call timing: In stage2.S, after VESA and before entering protected mode
 */

.section .text
.code16                          // 16-bit real mode code
```

**解释**：

这个文件包含两个关键函数：`query_memory_map`（E820 内存探测）和 `load_kernel_from_disk`（磁盘读取）。它们必须在 Real Mode 下调用，所以使用 `.code16` 指令。

---

### Step 2：定义 E820 相关常量

**目标**：定义 E820 内存布局和 BIOS 调用常量

**代码**（文件路径：`boot/common/boot.S`，第 22 行）：

```asm
// ============================================================
// Constant definitions (consistent with boot_info.h)
// ============================================================

// E820 memory layout
.set E820_BUFFER_ADDR,          0x5000
.set E820_BUFFER_COUNT_ADDR,    0x5000
.set E820_BUFFER_ENTRIES_ADDR,  0x5004
.set E820_MAX_ENTRIES,          32
.set E820_ENTRY_SIZE,           24

// Pre-calculated segment/offset values
// Real mode: physical = segment << 4 + offset
// For physical 0x5000: segment = 0x0500 (0x0500 << 4 = 0x5000)
.set E820_COUNT_ADDR,           0x5000
.set E820_ENTRIES_ADDR,          0x5004
.set E820_COUNT_SEG,             0x0500      // 0x0500 << 4 = 0x5000
.set E820_COUNT_OFF,             0x0000
.set E820_ENTRIES_SEG,           0x0500      // 0x0500 << 4 = 0x5000
.set E820_ENTRIES_OFF,           0x0004      // 0x5000 + 4 = 0x5004

// MemoryMapEntry structure offsets (24 bytes/entry)
.set MEM_MAP_BASE,            0
.set MEM_MAP_LENGTH,          8
.set MEM_MAP_TYPE,            16
.set MEM_MAP_SIZE,            24

// E820 BIOS call constants
.set E820_SIGNATURE,          0x534D4150  // 'SMAP'
.set E820_CMD,                0x0000E820  // E820 function ID (full 32-bit)
.set E820_MAX_BIOS_SIZE,      20          // Max entry size BIOS returns
```

**解释**：

这里定义了 E820 缓冲区的内存布局。我们把内存地图存放在物理地址 `0x5000`，前 4 字节存放条目数量，之后每 24 字节是一条 E820 记录。

⚠️ **特别注意**：Real Mode 的地址计算公式是 `物理地址 = 段 << 4 + 偏移`。要访问物理地址 `0x5000`，需要设置段 `0x0500`（因为 `0x0500 << 4 = 0x5000`），偏移为 `0`。我之前就踩过这个坑，直接用 `movw $0x5000, %es` 结果访问了 `0x50000`！

`E820_SIGNATURE = 'SMAP'` 是 BIOS 返回的签名，用于验证调用成功。

---

### Step 3：实现 E820 内存枚举函数

**目标**：编写 `query_memory_map` 函数，循环调用 BIOS 获取内存地图

**代码**（文件路径：`boot/common/boot.S`，第 89 行）：

```asm
// ============================================================
// Function: query_memory_map
// Responsibility: Execute E820 memory enumeration, store results in 0x5000 buffer
// Input: None
// Output: %cx = number of memory map entries
// Clobbers: %ax, %bx, %cx, %dx, %si, %di, %es (saved/restored)
// ============================================================
.global query_memory_map
.type query_memory_map, @function
query_memory_map:
    pushaw
    pushw %es
    pushw %ds

    // 初始化条目计数器为 0
    movw $0x0, %ax
    movw $E820_COUNT_SEG, %dx
    movw %dx, %ds
    movw %ax, E820_COUNT_OFF     // count = 0

    // 设置 ES:DI 指向缓冲区
    movw $E820_ENTRIES_SEG, %ax
    movw %ax, %es
    movw $E820_ENTRIES_OFF, %di

    // EBX = 0（第一次调用）
    xorl %ebx, %ebx

.e820_loop:
    // 检查是否超过最大条目数
    movl E820_COUNT_OFF, %eax
    cmpl $E820_MAX_ENTRIES, %eax
    jae .e820_done

    // EAX=0x0000E820, EBX=continuation, ECX=bufsize, EDX='SMAP', ES:DI=buffer
    movl $E820_SIGNATURE, %edx       // EDX = 'SMAP'
    movl $E820_CMD, %eax             // EAX = 0x0000E820
    movl $E820_ENTRY_SIZE, %ecx      // ECX = 24 (buffer size)

    int $0x15                        // 调用 BIOS E820
    jc .e820_failed                  // CF=1 表示失败

    cmpl $E820_SIGNATURE, %eax       // 检查返回签名
    jne .e820_failed

    // BIOS 可能返回 <24，至少要有 20
    cmpl $20, %ecx
    jb .e820_failed

    // 移动到下一个条目
    addl $E820_ENTRY_SIZE, %edi

    // 递增条目计数
    movl E820_COUNT_OFF, %eax
    incl %eax
    movl %eax, E820_COUNT_OFF

    // 检查是否还有更多条目
    testl %ebx, %ebx
    jnz .e820_loop

.e820_done:
    movl E820_COUNT_OFF, %eax
    movl %eax, %ecx

    popw %ds
    popw %es
    popaw
    ret

.e820_failed:
    popw %ds
    popw %es
    popaw

    movw $msg_e820_failed, %si
    jmp panic
```

**解释**：

这个函数的核心是 `.e820_loop` 循环。每次循环：
1. 检查是否超过最大条目数（防止缓冲区溢出）
2. 设置 E820 调用参数（EAX、EDX、ECX、ES:DI）
3. 调用 INT 0x15
4. 检查返回值（CF 标志、EAX 签名、ECX 大小）
5. 移动 DI 到下一个条目位置
6. 递增条目计数
7. 检查 EBX 是否为 0（是否还有更多条目）

⚠️ **常见陷阱**：
* **E820_CMD 必须是 0x0000E820**，不能只写 0xE820！高 16 位必须为 0。
* **ECX 必须是 24**，我们给的结构大小。BIOS 可能返回 20，但至少要 20。
* **EBX 初始为 0**，这是"第一次调用"的标志。

**验证**：可以在循环中添加 debugcon 输出：
```asm
movb $'E', %al
outb %al, $0xe9        // 每找到一条记录输出 'E'
```

这样你可以看到 BIOS 返回了几条记录。正常 QEMU 配置会返回 6-7 条。

---

### Step 4：定义磁盘读取常量和 DAP 结构

**目标**：定义 INT 13h 扩展读取所需的常量和 DAP（Disk Address Packet）结构

**代码**（文件路径：`boot/common/boot.S`，第 56 行）：

```asm
// DAP (Disk Address Packet) structure offsets (16 bytes)
.set DAP_SIZE,                0           // Packet size (bytes)
.set DAP_RESERVED,            1           // Reserved (always 0)
.set DAP_COUNT,               2           // Number of sectors to transfer
.set DAP_BUFFER_OFFSET,       4           // Transfer buffer offset (16-bit)
.set DAP_BUFFER_SEGMENT,      6           // Transfer buffer segment (16-bit)
.set DAP_LBA,                 8           // Starting LBA (64-bit)

// DAP fixed location in low memory (MBR DAP area, safe to reuse after MBR)
.set DAP_PHYS_ADDR,           0x7B00      // Fixed DAP location (physical address)
.set DAP_SEGMENT,             0x07B0      // DAP segment = 0x7B00 >> 4
.set DAP_OFFSET,              0x0000      // DAP offset

// Disk read constants
.set MINI_KERNEL_LBA,         16          // Mini kernel start LBA (sector 16)
.set KERNEL_LOAD_SEGMENT,     0x1000      // 0x10000 = 0x1000:0x0000
.set KERNEL_LOAD_OFFSET,      0x0000

.set DISK_READ_CMD,           0x42        // INT 0x13 AH=0x42 extended read
```

**解释**：

这里定义了 DAP 结构的偏移量。DAP 必须放在内存中，我们选择 `0x7B00`（MBR 使用的 DAP 区域，MBR 加载完成后可以安全重用）。

`MINI_KERNEL_LBA = 16` 表示小内核从磁盘第 16 扇区开始（前 16 扇区是 Bootloader）。`KERNEL_LOAD_SEGMENT = 0x1000` 表示目标地址是 `0x10000`（因为 `0x1000 << 4 = 0x10000`）。

---

### Step 5：实现磁盘读取函数

**目标**：编写 `load_kernel_from_disk` 函数，使用 INT 13h 扩展读取

**代码**（文件路径：`boot/common/boot.S`，第 166 行）：

```asm
// ============================================================
// Function: load_kernel_from_disk
// Responsibility: Read kernel ELF header (4KB) from disk LBA=16 to 0x10000
// Input: None
// Output: %ax = number of sectors read (8), 0 means failure
// Clobbers: %ax, %bx, %cx, %dx, %si, %di (saved/restored)
// ============================================================
.global load_kernel_from_disk
.type load_kernel_from_disk, @function
load_kernel_from_disk:
    pushaw                               // 保存所有通用寄存器
    pushw %es                            // 保存 ES
    pushw %ds                            // 保存 DS

    // 设置 ES 指向 DAP 段
    movw $DAP_SEGMENT, %ax               // ES = 0x07B0
    movw %ax, %es

    // Step 1: 构建 DAP 结构
    movw $DAP_OFFSET, %di                // DI = 0x0000

    // Step 2: 填充 DAP.size = 16（DAP 结构大小）
    movb $16, %es:(%di)                  // 设置 DAP size

    // Step 3: 填充 DAP.count = 8（4KB = 8 sectors）
    movw $8, %es:2(%di)                  // 设置扇区数量为 8

    // Step 4: 填充 DAP.buffer = 0x10000（segment:offset 格式）
    // offset = 0x0000
    movw $0x0000, %es:4(%di)             // buffer offset
    // segment = 0x1000
    movw $0x1000, %es:6(%di)             // buffer segment

    // Step 5: 填充 DAP.lba = MINI_KERNEL_LBA (16)
    movl $MINI_KERNEL_LBA, %eax          // LBA=16
    movl %eax, %es:8(%di)                // 设置起始 LBA（低 32 位）
    xorl %eax, %eax                      // 高 32 位 = 0
    movl %eax, %es:12(%di)               // LBA 高 32 位

    // Step 6: 调用 INT 0x13 AH=0x42 扩展读取
    // DS:SI 必须指向 DAP (0x07B0:0x0000 = 物理 0x7B00)
    movw $DAP_SEGMENT, %dx               // DX = 0x07B0
    movw %dx, %ds                        // DS = 0x07B0
    movw $DAP_OFFSET, %si                // SI = 0x0000
    movb $0x80, %dl                      // DL = 0x80（第一块硬盘）⚠️ 关键！
    movb $DISK_READ_CMD, %ah             // AH=0x42
    int $0x13                            // 执行磁盘读取

    // Step 7: 检查错误
    // CF=1 或 AH!=0 表示失败
    jc disk_read_failed                  // CF=1 表示失败
    cmpb $0, %ah                         // AH 应该为 0
    jne disk_read_failed                 // AH!=0 表示错误

    // Step 8: 恢复寄存器并返回扇区数量（8）
    popw %ds
    popw %es
    popaw
    movw $8, %ax                         // 返回 8 个扇区
    ret

disk_read_failed:
    popw %ds
    popw %es
    popaw
    movw $(msg_disk_read_failed), %si
    jmp panic
```

**解释**：

这个函数的核心是构建 DAP 结构，然后调用 INT 0x13。有几个关键点：

1. **DAP 构建步骤**：按顺序填充 size、count、buffer、lba 字段
2. **Buffer 地址格式**：`段:偏移`，`0x10000` 分解为 `0x1000:0x0000`
3. **DL 寄存器必须为 0x80**：这是"第一块硬盘"的设备号！我之前踩过这个坑，DL 没设置导致 BIOS 返回 AH=1 错误。

⚠️ **常见陷阱**：
* **DL 必须设置为 0x80**，否则 BIOS 会认为设备类型错误
* **DAP 结构必须完整**，16 字节一个都不能少
* **检查 CF 和 AH 两个标志**，CF 是"操作失败"标志，AH 是错误码

**验证**：可以用 GDB 检查读取结果：
```
(gdb) x/10i 0x10000
```

应该看到 ELF header 的开头：`7F 45 4C 46`（`\x7FELF`）。

---

### Step 6：在 Stage2 中调用新函数

**目标**：在 `boot/stage2.S` 中添加对 E820 和磁盘读取的调用

**代码**（文件路径：`boot/stage2.S`，第 36 行）：

首先添加外部函数声明：

```asm
// ============================================================
// External functions from common/boot.S (004_boot_load_mini_kernel_A)
// ============================================================
.extern query_memory_map
.extern load_kernel_from_disk
```

**解释**：

这告诉汇编器这两个函数在外部定义，稍后链接时会解析。

---

**代码**（文件路径：`boot/stage2.S`，第 113 行）：

在 VESA 初始化之后、进入保护模式之前添加调用：

```asm
    // ============================================================
    // 004_boot_load_mini_kernel_A: Real mode 内完成
    // ============================================================

    call query_memory_map           // [→0x5000] E820 memory map
    call load_kernel_from_disk      // [→0x10000] Load mini kernel ELF header

    cli                           // 再次关闭中断
    // ============================================================
    // Switch to Protected Mode
    // ============================================================
```

**解释**：

这里的关键是**调用时机**：必须在进入保护模式之前！因为 BIOS 中断只能在 Real Mode 下使用。顺序是：
1. VESA 初始化（已完成）
2. E820 内存探测（新增）
3. 磁盘读取（新增）
4. 关闭中断
5. 进入保护模式（下一步）

⚠️ **注意**：调用 `load_kernel_from_disk` 后，内核 ELF header 已经在 `0x10000`，但我们这一章不会解析它——那留到下一章。

---

### Step 7：创建 Mini Kernel 占位实现

**目标**：创建最小化的 ELF64 小内核，用于测试加载

**代码**（文件路径：`kernel/mini/main.cpp`）：

```cpp
/* ==============================================================
 * Cinux Mini Kernel - Minimal ELF for Disk Load Testing
 * ==============================================================
 *
 * Absolute minimal ELF to test bootloader disk reading.
 * Just halts.
 */

extern "C" {
[[noreturn]] void _start() {
    while (1) {
        __asm__ volatile(
            "cli; \
             hlt");
    }
}
}
```

**解释**：

这是"最小可用"的 64 位 ELF 程序。它只做一件事：无限循环 `cli; hlt`（关闭中断、停机）。但关键是它的格式是正确的 ELF64，Bootloader 可以正确读取和识别它。

下一章我们会扩展这个 mini kernel，添加真正的代码。

---

### Step 8：更新构建系统

**目标**：修改 CMakeLists.txt，编译新的模块

**代码**（文件路径：`boot/CMakeLists.txt`）：

添加公共 Boot 模块：

```cmake
# ==============================================================
# Common Boot Functions (E820 + Disk Read)
# ==============================================================

add_library(boot_common OBJECT
    common/boot.S
    common/serial.S
)

target_compile_options(boot_common PRIVATE
    -Wa,--32                    # 生成 32 位对象（包含 .code16, .code32）
)
```

**解释**：

我们把 `boot.S` 和 `serial.S` 编译成一个 Object Library，然后链接到 stage2。

---

**代码**（文件路径：`boot/CMakeLists.txt`）：

更新 stage2 依赖：

```cmake
add_executable(stage2
    stage2.S
    $<TARGET_OBJECTS:boot_common>       # 公共函数（含 E820 + 磁盘读取）
    $<TARGET_OBJECTS:boot_vesa>         # VESA 函数
    $<TARGET_OBJECTS:boot_longmode>     # Long Mode 函数
)
```

**解释**：

`stage2` 现在依赖于三个 Object Library：`boot_common`（新增）、`boot_vesa`、`boot_longmode`。

---

**代码**（文件路径：`kernel/CMakeLists.txt`）：

添加 mini kernel 目标：

```cmake
# ==============================================================
# Mini Kernel (用于测试磁盘加载)
# ==============================================================

add_executable(mini_kernel
    mini/main.cpp
)

target_compile_options(mini_kernel PRIVATE
    ${KERNEL_COMPILE_OPTIONS}
)

target_link_options(mini_kernel PRIVATE
    -T ${CMAKE_SOURCE_DIR}/kernel/linker.ld
    -nostdlib
)
```

**解释**：

mini_kernel 使用和主内核一样的编译选项，但链接到独立的位置。

---

**代码**（文件路径：`scripts/build_image.sh`）：

更新镜像构建脚本：

```bash
# Stage2: sectors 1-15 (max 15 sectors = 7680 bytes)
dd if=build/boot/stage2.bin of=cinux.img bs=512 count=15 seek=1 conv=notrunc 2>/dev/null

# Mini Kernel: sectors 16-... (LBA = 16)
# 计算实际需要的扇区数
MINI_KERNEL_SIZE=$(stat -f%z build/kernel/mini_kernel.bin 2>/dev/null || stat -c%s build/kernel/mini_kernel.bin 2>/dev/null)
MINI_KERNEL_SECTORS=$(( (MINI_KERNEL_SIZE + 511) / 512 ))

dd if=build/kernel/mini_kernel.bin of=cinux.img bs=512 count=${MINI_KERNEL_SECTORS} seek=16 conv=notrunc 2>/dev/null
```

**解释**：

这里我们把 mini_kernel 从 LBA=16 开始写入。`seek=16` 表示跳过前 16 扇区（MBR + stage2）。

---

### Step 9：输出调试字符

**目标**：在关键位置输出 debugcon 字符，便于调试

**代码**（文件路径：`boot/stage2.S`，在 Long Mode 入口处）：

```asm
long_mode_entry:
    // GDT 已经在 enter_long_mode 中加载

    // 设置数据段寄存器
    movw $GDT_DATA64, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    // 设置 64 位栈
    movabsq $0x90000, %rsp

    // 输出 'L' 表示 Long Mode
    movb $CHAR_LONG_MODE, %al
    outb %al, $DEBUGCON_PORT

    // 输出 'J' 表示即将跳转到小内核
    movb $'J', %al
    outb %al, $DEBUGCON_PORT

    // 暂停（下一章会真正跳转）
    cli
.lm_halt:
    hlt
    jmp .lm_halt
```

**解释**：

这里添加了 `'J'` 字符输出，表示"准备跳转"（Jump）。这一章我们只是暂停，下一章才会真正实现跳转。

---

## 五、编译与运行

### 5.1 编译命令

```bash
# 从项目根目录
git checkout 004_boot_load_mini_kernel_A
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Debug -B . -S ..
make
```

### 5.2 运行方法

```bash
# 运行 QEMU（带 debugcon）
qemu-system-x86_64 -drive format=raw,file=cinux.img -debugcon stdio
```

QEMU 参数说明：
* `-drive format=raw,file=cinux.img`：使用原始磁盘镜像
* `-debugcon stdio`：将 debugcon 输出到标准输出

### 5.3 预期输出

如果一切正常，你应该在终端看到：

```
Stage2 OK
Mode info OK, switching...
PLJ
```

* `Stage2 OK`：Stage2 启动成功
* `Mode info OK, switching...`：VESA 模式设置成功
* `P`：进入保护模式
* `L`：进入 Long Mode
* `J`：准备跳转到小内核（本章新增）

如果只看到 `PL` 然后崩溃，说明 E820 或磁盘读取有问题。

---

## 六、调试技巧

### 6.1 常见问题与排查方法

**问题 1：输出 `P` 后崩溃（进入保护模式失败）**

可能原因：
* E820 调用破坏了寄存器
* 磁盘读取修改了关键内存区域

排查方法：
```gdb
(gdb) break query_memory_map
(gdb) break load_kernel_from_disk
(gdb) continue
```

确保这两个函数正确返回。

**问题 2：看到 "E820: Memory query failed!"**

可能原因：
* EAX 不是 `0x0000E820`（高 16 位必须为 0）
* ECX 不是 24
* EDX 不是 `'SMAP'`

排查方法：
```gdb
(gdb) break query_memory_map
(gdb) continue
(gdb) info registers eax edx ecx
```

检查寄存器值是否符合要求。

**问题 3：看到 "DISK: Failed to read kernel!"**

可能原因：
* DL 不是 `0x80`
* DAP 结构错误
* LBA 超出磁盘范围

排查方法：
```gdb
(gdb) x/16bx 0x7B00   # 检查 DAP 结构
(gdb) info registers dl ah
```

确认 DAP 格式：`10 00 08 00 00 00 00 10 10 00 00 00 00 00 00 00`

**问题 4：ELF header 不正确**

可能原因：
* build_image.sh 中的 LBA 设置错误
* mini_kernel 没有正确编译

排查方法：
```bash
# 检查 mini_kernel 是否正确生成
file build/kernel/mini_kernel.bin
hexdump -C build/kernel/mini_kernel.bin | head

# 检查镜像中是否正确写入
hexdump -C cinux.img | grep "00002000"   # LBA 16 = 0x2000
```

应该看到 ELF magic number：`7f 45 4c 46`。

### 6.2 使用 GDB 调试

启动 QEMU 时添加 `-s -S` 参数：

```bash
qemu-system-x86_64 -drive format=raw,file=cinux.img -debugcon stdio -s -S
```

然后另一个终端启动 GDB：

```bash
gdb build/cinux.elf
(gdb) target remote :1234
(gdb) break stage2.S:117    # E820 调用位置
(gdb) continue
```

检查 E820 结果：
```gdb
(gdb) x/10wx 0x5000         # 查看内存地图
(gdb) print/x *(int*)0x5000 # 条目数量
```

检查磁盘读取结果：
```gdb
(gdb) x/10i 0x10000         # 查看 ELF header
(gdb) x/16bx 0x7B00         # 查看是否有 'E' 'L' 'F'
```

### 6.3 Debugcon 验证技巧

在 E820 循环中添加字符输出：
```asm
// 在 .e820_loop 中
movb $'E', %al
outb %al, $0xe9        // 每找到一条记录输出 'E'
```

在磁盘读取后添加字符输出：
```asm
// 在 load_kernel_from_disk 成功后
movb $'D', %al
outb %al, $DEBUGCON_PORT    // 'D' 表示磁盘读取成功
```

这样可以看到程序执行到哪一步。

---

## 七、本章小结

### 7.1 新增关键函数/结构

| 名称 | 类型 | 位置 | 功能 |
|------|------|------|------|
| `query_memory_map` | 函数 | `boot/common/boot.S` | 执行 E820 内存枚举 |
| `load_kernel_from_disk` | 函数 | `boot/common/boot.S` | 从磁盘读取 ELF header |
| `MemoryMapEntry` | 结构（逻辑） | `0x5000` 缓冲区 | E820 内存条目格式 |
| `DAP` | 结构 | `0x7B00` | INT 13h 扩展读取参数包 |

### 7.2 新增中断/BIOS 调用

| 中断 | 参数 | 功能 |
|------|------|------|
| `INT 0x15, AX=0xE820` | EBX=续接值, ECX=24, EDX='SMAP' | E820 内存探测 |
| `INT 0x13, AH=0x42` | DS:SI→DAP, DL=0x80 | 扩展磁盘读取 |

### 7.3 内存布局

| 地址 | 用途 | 大小 |
|------|------|------|
| `0x5000` | E820 条目数量（4 字节） | 4 B |
| `0x5004` | E820 条目数组 | 最大 768 B (32×24) |
| `0x7B00` | DAP 结构（INT 13h 参数） | 16 B |
| `0x10000` | Mini kernel ELF header | 4 KB |

### 7.4 下一步

下一章我们将完成真正的 ELF 加载和跳转：

1. 解析 ELF64 Program Header
2. 加载内核代码段到正确位置
3. 设置高半内核映射（`0xFFFFFFFF80000000`）
4. 跳转到内核入口点

届时 `J` 字符之后会真正进入 C++ 内核代码！

---

## 八、参考资源

* [OSDev Wiki: Detecting Memory (x86)](https://wiki.osdev.org/Detecting_Memory_(x86)) —— E820 详细说明
* [OSDev Wiki: INT 13h]((https://wiki.osdev.org/INT_13h)) —— BIOS 磁盘接口文档
* [BIOS E820 Specification]((https://web.archive.org/web/20150618004740/http://www.ctech.ece.maine.edu/ftp/pub/cos440/int15h.txt)) —— E820 原始规范
* [ELF-64 Object File Format]((https://refspecs.linuxfoundation.org/elf/elf64.pdf)) —— ELF64 格式规范

---

## 附录：E820 内存地图示例

QEMU 默认配置的典型 E820 输出（7 条记录）：

```
条目 0: base=0x0000000000000000 len=0x000000000009FC00 type=1 (可用，约 640KB)
条目 1: base=0x000000000009FC00 len=0x0000000000000400 type=2 (保留，EBDA)
条目 2: base=0x00000000000E0000 len=0x0000000000020000 type=2 (保留，ROM area)
条目 3: base=0x0000000000100000 len=0x000000001FEE0000 type=1 (可用，约 511MB)
条目 4: base=0x000000001FFE0000 len=0x0000000000020000 type=2 (保留，32MB-32MB+128KB)
条目 5: base=0x00000000FFFC0000 len=0x0000000000004000 type=2 (保留，BIOS)
条目 6: base=0x0000000FD0000000 len=0x0000000003000000 type=2 (保留，64位 PCIe)
```

只有 Type=1 的条目（0 和 3）是真正的可用内存，可以直接使用。
