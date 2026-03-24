# 004_boot_load_mini_kernel_B · 加载小内核（数据结构篇）

> 本章完成后的可见效果：QEMU 不崩溃，debugcon 输出 `J`（确认即将跳转小内核），随后小内核接管
>
> 前置要求：已完成 `004_boot_load_mini_kernel_A`，理解 E820 内存探测和磁盘读取基础

---

## 一、前言：定义 Bootloader 和内核的"契约"

说实话，上一章我们搞定了 E820 内存探测和磁盘读取，但留下了一个悬而未决的问题：bootloader 和内核之间怎么传递信息？你可以把这个问题想象成两个陌生人交接工作——如果没有一个标准的"交接清单"，内核拿到内存后完全不知道状况。

这一章我们要做的就是定义这份"交接清单"：`BootInfo` 结构体。它既是 bootloader 写的"工作报告"，也是内核读的"入职指南"。更重要的是，这个结构体会被 bootloader（16/32 位汇编）和内核（64 位 C++）共同使用，所以它的布局必须在两种编译模式下完全一致。

**这一步之后，我们就能：**
- 用一个统一的数据结构在 bootloader 和内核之间传递信息
- 确保 32 位和 64 位编译模式下数据布局一致
- 为后续完整的内核跳转铺好路

---

## 二、核心概念精讲

### 2.1 为什么需要 BootInfo？

你可能会问：bootloader 直接把数据写到固定地址，内核去读不就行了吗？这当然可以，但会带来几个问题：

* **硬编码地址容易出错**：如果将来内存布局调整，所有地方都要改
* **缺乏自描述性**：内核怎么知道 E820 有几条记录？ framebuffer 在哪？
* **跨编译风险**：32 位和 64 位的结构体布局可能不同

`BootInfo` 结构体就是答案：它把所有关键信息打包成一个结构，bootloader 填充后传递指针给内核。这样双方都只需要知道结构体定义，不需要记住一堆地址。

### 2.2 32 位与 64 位的数据布局一致性

这是最关键的一点！我们的 bootloader 编译成 32 位（`.code32` 部分），而内核是 64 位 C++。如果 `BootInfo` 在两种模式下的内存布局不一致，内核读到的就是垃圾数据。

确保一致性的规则：
* **使用固定大小类型**：`uint32_t`、`uint64_t`，不要用 `int` 或 `long`
* **显式对齐**：用 `__attribute__((packed))` 禁止编译器插入填充字节
* **显式 padding**：如果需要对齐，用明确的 `_pad` 字段

### 2.3 扁平二进制 vs ELF 格式

这里需要澄清一个架构选择：bootloader → 小内核使用**扁平二进制**（flat binary），而小内核 → 大内核使用 ELF 格式。

为什么不统一用 ELF？因为 Real Mode 下的 bootloader 太"弱"了：
* 解析 ELF 需要 64 位算术（ELF64 的字段都是 64 位）
* 需要处理 Program Header、重定位等复杂逻辑
* Real Mode 只有 16 位寄存器，处理 64 位值很麻烦

所以我们的加载策略是：
1. **Real Mode**：用 BIOS INT 13h 直接读完整小内核到 0x20000（flat binary）
2. **Long Mode**：小内核有完整的 C++ 运行环境，可以解析 ELF 加载大内核

### 2.4 内存布局约定

bootloader 和内核需要约定好数据的物理位置：

| 地址 | 用途 | 大小 |
|------|------|------|
| `0x5000` | E820 条目数量 + 内存地图 | 4 + 32×24 B |
| `0x6400` | VESA framebuffer 信息 | 16 B |
| `0x7000` | `BootInfo` 结构体 | ~800 B |
| `0x20000` | 小内核镜像 | ≤416 KB |

这些地址是"硬编码契约"，双方都要遵守。`BootInfo` 本身放在 0x7000，但其中的 `mmap` 数组指向 0x5000 的数据，`fb_addr` 等字段来自 0x6400 的 VESA 信息。

---

## 三、动手实现

### Step 1：创建 BootInfo 头文件

**目标**：创建 `boot/boot_info.h`，定义 bootloader 和内核共用的数据结构

**代码**（文件路径：`boot/boot_info.h`）：

```c
/**
 * @file boot/boot_info.h
 * @brief Bootloader-to-kernel handoff structure definition
 *
 * This file defines the BootInfo structure that BOTH the bootloader AND the kernel
 * must use to pass information from the boot process to the kernel.
 *
 * IMPORTANT: This header is included by:
 *   - Bootloader C code (boot/elf_loader.c) - compiled with -m32
 *   - Kernel C++ code (kernel/mini/main.cpp) - compiled with -m64
 *
 * The structure layout and field types MUST be identical between 32-bit and 64-bit
 * compilation to ensure correct data interpretation. All fields use explicitly-sized
 * types (uint32_t, uint64_t) and padding is explicit to avoid ABI differences.
 */

#ifndef BOOT_BOOT_INFO_H
#define BOOT_BOOT_INFO_H

#include <stdint.h>
```

**解释**：

这个头文件的注释非常关键，它直接说明了"为什么这样设计"。特别注意 `compiled with -m32` 和 `compiled with -m64` 的注释——提醒使用者这是一个跨编译模式使用的结构。

---

### Step 2：定义 MemoryMapEntry 结构

**目标**：定义 E820 内存条目的 C 结构表示

**代码**（文件路径：`boot/boot_info.h`，第 33 行）：

```c
// ============================================================
// Memory Map Entry (from E820 BIOS call)
// ============================================================
// Layout matches E820 BIOS returned format:
//   Base:   64-bit physical base address
//   Length: 64-bit region length in bytes
//   Type:   32-bit memory type (1=usable, 2=reserved, etc.)
//   ACPI:   32-bit ACPI attributes (extended E820, usually 0 for old BIOS)

/**
 * @brief Single memory map entry from E820 query
 *
 * Represents one contiguous memory region reported by BIOS E820 call.
 * Type values: 1=usable RAM, 2=reserved, 3=ACPI reclaimable, 4=ACPI NVS, etc.
 */
typedef struct {
    uint64_t base;          // Physical base address of the region
    uint64_t length;        // Region length in bytes
    uint32_t type;          // Memory type (1=usable, 2=reserved, etc.)
    uint32_t acpi;          // ACPI extended attributes (usually 0)
} __attribute__((packed)) MemoryMapEntry;

// Static assertion: ensure struct size matches E820 format (24 bytes)
static_assert(sizeof(MemoryMapEntry) == 24, "MemoryMapEntry must be 24 bytes");
```

**解释**：

`MemoryMapEntry` 完美对应 E820 BIOS 返回的 24 字节格式。每个字段的大小都和 BIOS 返回的一致：`base` 和 `length` 是 64 位，`type` 和 `acpi` 是 32 位。

`__attribute__((packed))` 是关键！它告诉 GCC"不要插入任何填充字节"，确保结构体精确等于 24 字节。`static_assert` 在编译时验证这一点，如果大小不对会直接报错。

⚠️ **常见陷阱**：如果不加 `packed`，GCC 可能会在 `acpi` 后插入 4 字节填充，导致结构体变成 28 字节！

---

### Step 3：定义 BootInfo 结构体

**目标**：定义完整的 bootloader 到内核交接结构

**代码**（文件路径：`boot/boot_info.h`，第 63 行）：

```c
// ============================================================
// Boot Information Structure
// ============================================================
// Placed at physical 0x7000 by bootloader before kernel jump.
// Kernel receives this as first argument (in %rdi per System V AMD64 ABI).

/**
 * @brief Complete boot information passed from bootloader to kernel
 *
 * This structure contains all information collected by the bootloader:
 * - Kernel entry point and load information
 * - Framebuffer details for graphics output
 * - Memory map for physical memory management
 *
 * The bootloader fills this in protected mode (stage2.S), then passes
 * a pointer to it as the first argument when jumping to the kernel.
 */
typedef struct {
    // Kernel information
    uint64_t entry_point;       // Virtual entry point address (high-half kernel address)
    uint64_t kernel_phys_base;  // Physical base address where kernel ELF was loaded (0x20000)
    uint64_t kernel_size;       // Actual ELF file size in bytes

    // Framebuffer information (from VESA BIOS calls, stored at 0x6400)
    uint64_t fb_addr;           // Physical framebuffer base address
    uint32_t fb_width;          // Framebuffer width in pixels
    uint32_t fb_height;         // Framebuffer height in pixels
    uint32_t fb_pitch;          // Bytes per scan line (may be > width * bpp)
    uint32_t fb_bpp;            // Bits per pixel (usually 32)

    // Memory map (from E820 BIOS call, stored at 0x5000)
    uint32_t mmap_count;        // Number of valid entries in mmap[] array
    uint32_t _pad;              // Explicit padding for alignment
    MemoryMapEntry mmap[32];    // Memory map entries (max 32 entries)

} __attribute__((packed)) BootInfo;

// Static assertion: ensure BootInfo layout is predictable
static_assert(sizeof(BootInfo) == 24 + 32 + 8 + 4 + 4 + 4 + 4 + 4 + (32 * 24),
              "BootInfo size mismatch");
```

**解释**：

这个结构体是本章的核心。它包含三类信息：

1. **内核信息**：入口点、物理基址、大小。注意 `entry_point` 是**虚拟地址**（高半核），不是物理地址。这是因为我们会使用高半核映射（`0xFFFFFFFF80000000` + 物理地址）。

2. **Framebuffer 信息**：这些数据来自 VESA BIOS 调用（001 章节实现），存放在 0x6400。bootloader 需要把这些值复制到 `BootInfo` 中。

3. **内存地图**：E820 返回的数据存放在 0x5000，这里用 `mmap[32]` 数组预留空间。`mmap_count` 记录有效条目数。

`_pad` 字段是显式的对齐填充。在 `mmap_count`（4 字节）后，我们需要让 8 字节的 `mmap[0]` 对齐到 8 字节边界，所以插入 4 字节 padding。

⚠️ **注意**：`static_assert` 中的大小计算公式是：`内核字段(24B) + framebuffer字段(20B) + mmap_count(4B) + _pad(4B) + mmap数组(32×24B) = 24 + 20 + 4 + 4 + 768 = 820B`。

---

### Step 4：更新 Real Mode 磁盘读取（完整小内核）

**目标**：修改 `boot/common/boot.S` 中的 `load_kernel_from_disk`，读取完整小内核到 0x20000

上一章我们只读取了 4KB ELF header，这一步要改成读取完整的小内核镜像（flat binary）。

**代码**（文件路径：`boot/common/boot.S`，第 70 行）：

```asm
// ============================================================
// Mini kernel loading configuration
// ============================================================
// Real mode can only access low 1MB directly, so we load to 0x20000
// The mini kernel (512KB) fits in 0x10000 ~ 0x90000
// After entering long mode, the disk driver will relocate the large kernel

// Mini kernel disk location
.set MINI_KERNEL_LBA,         16          // Mini kernel start LBA (sector 16)
.set MINI_KERNEL_SECTORS,     832         // Total sectors (416KB) - 32KB gap before protected mode stack at 0x90000

// Mini kernel load address (real mode accessible: < 1MB)
// Real mode stack:    SS=0x0900, SP=0xFFFE (physical: 0x9000~0x19000)
// Protected mode stack: ESP=0x90000 (stage2.S line 168)
// Kernel loads to 0x20000~0x90000 to avoid both stacks
.set MINI_KERNEL_LOAD_PHYS,   0x20000     // Physical address where kernel is loaded
.set MINI_KERNEL_LOAD_SEG,    0x2000      // Segment: 0x2000 (0x2000 << 4 = 0x20000)
.set MINI_KERNEL_LOAD_OFF,    0x0000      // Offset
```

**解释**：

这里定义了小内核的加载参数。`MINI_KERNEL_SECTORS = 832` 表示最多 416KB（832 × 512）。这个限制是由内存布局决定的：

* Real mode stack: `0x9000` ~ `0x19000`（约 64KB）
* 小内核加载区: `0x20000` ~ `0x90000`（416KB）
* Protected mode stack: `0x90000` 开始

这样设计确保三个区域不重叠。

⚠️ **注意**：`MINI_KERNEL_LOAD_SEG = 0x2000`，因为 `0x2000 << 4 = 0x20000`。Real Mode 的地址计算公式是 `物理地址 = 段 << 4 + 偏移`。

---

**代码**（文件路径：`boot/common/boot.S`，第 191 行，完整函数）：

```asm
// ============================================================
// Function: load_kernel_from_disk
// Responsibility: Read complete mini kernel ELF (512KB) from disk LBA=16
//                 directly to MINI_KERNEL_LOAD_PHYS (0x20000)
//
// Strategy: Read in chunks (max 127 sectors per BIOS call)
//   - Each iteration updates DAP buffer address for next chunk
//   - No intermediate copy needed: direct read to final location
//
// Memory layout after load:
//   0x20000 ~ 0x90000: Mini kernel (512KB)
//
// Input: None
// Output: %ax = number of sectors read (1024), 0 means failure
// Clobbers: %ax, %bx, %cx, %dx, %si, %di, %es (saved/restored)
// ============================================================
.global load_kernel_from_disk
.type load_kernel_from_disk, @function
load_kernel_from_disk:
    pusha                                // [regs->stack] save all 16-bit registers
    pushw %es                            // [es->stack] save ES
    pushw %ds                            // [ds->stack] save DS

    // Initialize: BX = sectors read so far, SI = current buffer offset
    xorw %bx, %bx                        // [0->bx] BX = 0 (no sectors read yet)
    xorw %si, %si                        // [0->si] SI = 0 (buffer offset, stays 0)

    // Set ES to DAP segment for DAP access
    movw $DAP_SEGMENT, %dx               // [imm->dx] DX = 0x07B0
    movw %dx, %es

.read_loop:
    // Check if we've read all sectors
    cmpw $MINI_KERNEL_SECTORS, %bx       // [imm->bx] Compare with total
    jae .read_done                       // [cf->] Done if BX >= total

    // Calculate sectors to read this iteration (min(127, remaining))
    movw $MINI_KERNEL_SECTORS, %ax       // [imm->ax] AX = total sectors
    subw %bx, %ax                        // [bx->ax] AX = remaining sectors
    cmpw $DISK_MAX_SECTORS_PER_CALL, %ax // [imm->ax] Compare with max
    jbe .read_count_ok                   // [cf->] Use AX if <= 127
    movw $DISK_MAX_SECTORS_PER_CALL, %ax // [imm->ax] Cap at 127

.read_count_ok:
    // Save sector count in BP
    movw %ax, %bp                        // [ax->bp] BP = sectors to read

    // CRITICAL: Re-set ES before building DAP (BIOS may clobber it)
    movw $DAP_SEGMENT, %dx               // [imm->dx] DX = 0x07B0
    movw %dx, %es                        // [dx->es] ES = DAP segment

    // Build DAP at fixed location 0x7B00
    movw $DAP_OFFSET, %di                // [imm->di] DI = 0x0000

    // Fill DAP.size = 16
    movb $16, %es:(%di)                  // [imm->mem] set DAP size

    // Fill DAP.reserved = 0 (important for some BIOS)
    movb $0, %es:1(%di)                  // [imm->mem] clear reserved byte

    // Fill DAP.count = BP
    movw %bp, %es:2(%di)                 // [bp->mem] set sector count

    // Fill DAP.buffer = MINI_KERNEL_LOAD_PHYS + (BX * 512)
    // Each sector = 512 bytes, so offset = BX sectors * 512
    // In real mode: segment = (base >> 4), offset = (base & 0xF)
    // Base = 0x20000 + BX * 512
    //      = 0x20000 + (BX << 9)
    // Segment = (0x20000 + (BX << 9)) >> 4
    //        = 0x2000 + (BX << 5)
    //        = 0x2000 + (BX * 32)
    movw %bx, %ax                        // [bx->ax] AX = sector offset
    shlw $5, %ax                         // [ax->ax] AX *= 32
    addw $MINI_KERNEL_LOAD_SEG, %ax      // [imm->ax] AX = target segment
    movw %ax, %es:6(%di)                 // [ax->mem] buffer segment
    movw $MINI_KERNEL_LOAD_OFF, %ax      // [imm->ax] AX = 0 (offset always 0)
    movw %ax, %es:4(%di)                 // [ax->mem] buffer offset

    // Fill DAP.lba = MINI_KERNEL_LBA + BX (full 8 bytes, must be zero-initialized)
    movw $MINI_KERNEL_LBA, %ax           // [imm->ax] AX = base LBA (16)
    addw %bx, %ax                        // [bx->ax] AX = current LBA
    movw %ax, %es:8(%di)                 // [ax->mem] LBA low 16 bits
    xorw %ax, %ax                        // [0->ax] AX = 0
    movw %ax, %es:10(%di)                // [ax->mem] LBA high 16 bits
    movw %ax, %es:12(%di)                // [ax->mem] LBA upper 16 bits
    movw %ax, %es:14(%di)                // [ax->mem] LBA top 16 bits (clear garbage)

    // CRITICAL: Save BX and BP across BIOS call (BIOS clobbers both)
    pushw %bx                            // [bx->stack] save sector counter
    pushw %bp                            // [bp->stack] save sector count

    // Call INT 0x13 AH=0x42
    // CRITICAL: INT 13h AH=42h requires DS:SI to point to DAP, NOT ES:SI!
    //            DS was saved on stack at function entry, so we can modify it.
    movw $DAP_SEGMENT, %dx               // [imm->dx] DX = 0x07B0
    movw %dx, %ds                        // [dx->ds] DS = DAP segment (REQUIRED!)
    movw $DAP_OFFSET, %si                // [imm->si] SI = 0x0000
    movb $0x80, %dl                      // [imm->dl] DL = 0x80
    movb $DISK_READ_CMD, %ah             // [imm->ah] AH=0x42

    int $0x13                            // [->regs] execute disk read

    jc .disk_error_restore_bp            // [cf->] CF=1 means failure
    cmpb $0, %ah                         // [ah->] AH should be 0
    jne .disk_error_restore_bp           // [zf->] AH!=0 means error

    // CRITICAL: Restore BP and BX after BIOS call (success path)
    popw %bp                             // [stack->bp] restore sector count
    popw %bx                             // [stack->bx] restore sector counter

    // Update sector counter
    addw %bp, %bx                        // [bp->bx] BX += sectors read

    jmp .read_loop                       // [->] Continue reading

.disk_error_restore_bp:
    // Restore BP and BX before handling error (stack: BP then BX)
    popw %bp                             // [stack->bp] restore sector count
.disk_error_restore:
    popw %bx                             // [stack->bx] restore sector counter
    jmp disk_read_failed                 // [->] handle error

.read_done:
    popw %ds                             // [stack->ds] restore DS
    popw %es                             // [stack->es] restore ES
    popa
    movb $'O', %al                       // [imm->al] Load 'O' (0x4F)
    outb %al, $0xe9                      // [al->port] Output 'O' to debugcon

    movw $MINI_KERNEL_SECTORS, %ax       // [imm->ax] return sectors read
    ret                                  // [return] return

disk_read_failed:
    movb $'F', %al                       // [imm->al] Load 'F'
    outb %al, $0xe9                      // [al->port] Output 'F' to debugcon
    popw %ds                             // [stack->ds] restore DS
    popw %es                             // [stack->es] restore ES
    popa                                 // [stack->regs] restore general registers
    movw $(msg_disk_read_failed), %si    // [msg->si] load error message
    jmp panic                            // [->never] call panic
```

**解释**：

这个函数和上一章的版本相比，核心区别是**循环读取**而不是一次性读取。原因有二：

1. **BIOS INT 13h 限制**：单次调用最多读取 127 个扇区（这是 BIOS 规范限制）
2. **支持任意大小内核**：虽然我们当前限制是 832 扇区，但循环结构可以轻松扩展

循环的核心逻辑：
1. 计算本次读取扇区数：`min(127, 剩余扇区)`
2. 构建 DAP，buffer 地址动态计算：`0x20000 + (已读扇区 × 512)`
3. 调用 INT 13h 读取
4. 更新已读扇区计数，继续循环

**最关键的坑**：buffer 地址的段地址计算。公式是：
```
segment = 0x2000 + (sector_offset × 32)
```
推导过程：
* 物理地址 = `0x20000 + sector_offset × 512`
* 段地址 = 物理地址 >> 4 = `0x2000 + sector_offset × 32`
* 因为 `512 >> 4 = 32`

⚠️ **常见陷阱**：
* **DS:SI 必须指向 DAP**，不是 ES:SI！我之前踩过这个坑，用错了段寄存器导致 BIOS 返回 AH=1。
* **BIOS 会破坏寄存器**：必须保存 BX 和 BP，因为 BIOS 可能修改它们。
* **DAP 的 8 字节 LBA 必须清零**：如果高 32 位有垃圾值，某些 BIOS 会报错。

**验证**：成功后输出 `'O'`，失败输出 `'F'`。你应该在 debugcon 看到 `O`，表示读取成功。

---

### Step 5：在 Stage2 中调用加载函数（无操作版本）

**目标**：在 `boot/stage2.S` 的保护模式入口确认"无操作"

**代码**（文件路径：`boot/stage2.S`，第 149 行）：

```asm
// ============================================================
// Protected Mode Entry Point
// ============================================================
.code32                          // Now in 32-bit protected mode
pm_entry:
    // 4. Set up data segment registers
    movw $0x10, %ax               // Data selector value
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    // 5. Set up new stack
    movl $0x90000, %esp           // New stack in protected mode

    // 6. Debug output: 'P' for Protected Mode
    movb $0x50, %al               // 'P'
    outb %al, $0xE9               // Debugcon output

    // ============================================================
    // 004_boot_load_mini_kernel_B: No operation needed
    // ============================================================
    // Mini kernel bin was already loaded to 0x20000 in real mode.
    // Entry point is fixed at 0xFFFFFFFF80020000 (high-half kernel).
    // Nothing to do in protected mode, proceed to long mode.

    // ============================================================
    // Transition to Long Mode
    // ============================================================

    // Setup page tables for long mode
    call setup_page_tables          // [->mem] Setup page tables at 0x1000-0x3FFF

    // Enter long mode
    call enter_long_mode            // [->rip] Jump to 64-bit mode
```

**解释**：

这一步的重点是"什么都不做"。上一章（A 阶段）我们已经完成了所有 Real Mode 下的工作：
* E820 内存探测 → 数据存到 0x5000
* 磁盘读取 → 小内核加载到 0x20000

所以保护模式入口不需要做任何额外操作，直接跳到 Long Mode 初始化即可。下一章（C 阶段）我们会在 Long Mode 入口填充 `BootInfo` 并跳转。

**验证**：运行后应该看到 `POL`（Protected Mode、Long Mode，下一章加 J）。

---

## 四、构建与运行

### 4.1 编译命令

```bash
# 从项目根目录
git checkout 004_boot_load_mini_kernel_B
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Debug -B . -S ..
make
```

### 4.2 运行方法

```bash
# 运行 QEMU（带 debugcon）
qemu-system-x86_64 -drive format=raw,file=cinux.img -debugcon stdio
```

或者使用 CMake 目标：

```bash
make run
```

QEMU 参数说明：
* `-drive format=raw,file=cinux.img`：使用原始磁盘镜像
* `-debugcon stdio`：将 debugcon 输出到标准输出（终端）

### 4.3 预期输出

如果一切正常，你应该在终端看到：

```
Stage2 OK
Mode info OK, switching...
POL
```

* `Stage2 OK`：Stage2 启动成功
* `Mode info OK, switching...`：VESA 模式设置成功
* `P`：进入保护模式
* `O`：磁盘读取成功（本章新增）
* `L`：进入 Long Mode

如果看到 `POLF`，说明磁盘读取失败（F 表示 Failed）。

---

## 五、调试技巧

### 5.1 常见问题与排查方法

**问题 1：输出 `POF` 或 `PFL`（磁盘读取失败）**

可能原因：
* DAP 结构错误（特别是 buffer 段地址计算）
* DL 寄存器不是 0x80
* DS:SI 没有正确指向 DAP

排查方法：
```gdb
(gdb) break load_kernel_from_disk
(gdb) continue
(gdb) x/16bx 0x7B00   # 检查 DAP 结构
(gdb) info registers dl ah
```

确认 DAP 格式：`10 00 7F 00 00 00 00 20 ...`（size=16, count=127, buffer=0x20000）。

**问题 2：内核大小超过 416KB**

可能原因：
* 小内核代码量增长，超过预设限制

排查方法：
```bash
# 检查 mini_kernel.bin 大小
ls -lh build/kernel/mini/mini_kernel.bin
```

如果超过 416KB，需要：
1. 减小小内核代码量
2. 或调整内存布局（将 protected mode stack 上移）

**问题 3：输出 `PO` 后卡住**

可能原因：
* 循环读取逻辑错误
* BIOS 返回值检查错误

排查方法：
```gdb
(gdb) break .read_loop
(gdb) continue
(gdb) info registers bx bp
```

检查 BX（已读扇区）是否正确递增，BP（本次读取数）是否合理。

---

## 六、本章小结

### 6.1 新增关键数据结构

| 名称 | 类型 | 大小 | 功能 |
|------|------|------|------|
| `MemoryMapEntry` | 结构 | 24 B | E820 内存条目（base/length/type/acpi） |
| `BootInfo` | 结构 | ~820 B | Bootloader 到内核的交接结构 |

### 6.2 内存布局约定

| 地址 | 用途 |
|------|------|
| `0x5000` | E820 内存地图（数量 + 条目数组） |
| `0x6400` | VESA framebuffer 信息 |
| `0x7000` | `BootInfo` 结构体 |
| `0x20000` | 小内核镜像加载地址 |

### 6.3 下一步

下一章（`004_boot_load_mini_kernel_C`）将完成最后一步：

1. 在 Long Mode 入口填充 `BootInfo` 结构
2. 设置高半核入口点（`0xFFFFFFFF80020000`）
3. 跳转到小内核
4. 小内核输出 `'M'` 确认成功

届时 `POL` 会变成 `POLM`，表示真正的内核跳转完成！

---

## 七、参考资源

* [OSDev Wiki: Boot Information](https://wiki.osdev.org/Boot_Info) —— BootInfo 设计参考
* [System V AMD64 ABI](https://refspecs.linuxfoundation.org/elf/x86-64-abi-0.99.pdf) —— 寄存器参数传递规范（%rdi = 第一个参数）
* [GCC Attribute Syntax](https://gcc.gnu.org/onlinedocs/gcc/Common-Variable-Attributes.html) —— `packed` 属性说明
* [BIOS INT 13h Extensions](https://en.wikipedia.org/wiki/INT_13H) —— INT 13h AH=42h 详细说明
