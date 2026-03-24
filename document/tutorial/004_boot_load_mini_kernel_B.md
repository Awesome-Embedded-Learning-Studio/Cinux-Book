# Bootloader 开发日记 004B：保护模式的"静默"与合作之美

> 标签：x86, 保护模式, Long Mode, bootloader, 内存布局, 协作设计

---

## 本章概览

上一章我们花了大量篇幅讲解 Real Mode 下的两件大事：E820 内存枚举和磁盘读取。你可能会有一个疑问：**为什么在进入保护模式后，我们什么都不做？**

这个设计看似反直觉，但实际上是一个精妙的架构决策。Real Mode 完成了所有繁重的"搬运"工作，保护模式只是一个中转站，Long Mode 才是最终目的地。

**关键设计决策一览**：

- Real Mode 完成全部内核加载工作，包括 E820 枚举和磁盘读取
- 保护模式（Protected Mode）只作为模式切换的过渡，不做额外操作
- Long Mode 接收控制权后，只需要填充 BootInfo 并跳转到内核
- 内存布局在 Real Mode 阶段就已经确定，后续阶段无需修改

---

## 架构图

```
+---------------------------------------------------------------------+
|                        模式切换与数据流                               |
+---------------------------------------------------------------------+
|                                                                     |
|  Real Mode (stage2)                                                 |
|  │                                                                  |
|  ├─ E820 内存枚举        → [0x5000] MemoryMapEntry[]               |
|  ├─ 磁盘读取 Mini Kernel → [0x20000] ELF 数据 (416KB)               |
|  └─ 切换到保护模式                                                     |
|                                                                     |
|  Protected Mode (pm_entry)                                          |
|  │                                                                  |
|  └─ [什么都不做！] 只设置段寄存器和栈                                 |
|                                                                     |
|  Long Mode (long_mode_entry)                                       |
|  │                                                                  |
|  ├─ 填充 BootInfo        → [0x7000] BootInfo 结构                  |
|  ├─ 设置高半内核映射         → 页表更新                            |
|  └─ 跳转到内核入口           → jmp *entry_point                    |
|                                                                     |
+---------------------------------------------------------------------+

|                        Real Mode 完成的工作                           |
+---------------------------------------------------------------------+
|                                                                     |
|  E820 内存枚举 (query_memory_map)                                   |
|    ├─ 调用 BIOS INT 0x15/E820                                       |
|    ├─ 迭代获取所有内存区域                                          |
|    └─ 存储到 0x5000                                                |
|                                                                     |
|  磁盘读取 (load_kernel_from_disk)                                   |
|    ├─ 构建磁盘地址包 (DAP)                                          |
|    ├─ 调用 BIOS INT 0x13/AH=0x42                                    |
|    ├─ 循环读取 832 扇区 (416KB)                                     |
|    └─ 直接写入 0x20000                                              |
|                                                                     |
|  为什么在 Real Mode 完成？                                           |
|    ├─ BIOS 中断只能在 Real Mode 调用                                |
|    ├─ 实模式段地址计算简单直接                                       |
|    └─ 避免在保护模式处理 BIOS 兼容性                                 |
|                                                                     |
+---------------------------------------------------------------------+
```

---

## 环境说明

- **平台**：WSL2 + QEMU system-x86_64 6.2+
- **工具链**：GNU AS（AT&T 语法），ld 链接器
- **调试方式**：QEMU debugcon（端口 0xE9）写入 debug.log
- **前置要求**：已完成 004A 章节，E820 和磁盘读取已在 Real Mode 完成

---

## 第一阶段 —— 为什么保护模式"什么都不做"？

当我们进入保护模式后，`pm_entry` 函数的内容出奇的简单：

```asm
// file: boot/stage2.S

.code32                          // Now in 32-bit protected mode
pm_entry:
    // Set up data segment registers
    movw $0x10, %ax               // Data selector value
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    // Set up new stack
    movl $0x90000, %esp           // New stack in protected mode

    // Debug output: 'P' for Protected Mode
    movb $0x50, %al               // 'P'
    outb %al, $0xE9               // Debugcon output

    // [→next] 直接进入 Long Mode 初始化
    call setup_page_tables
    call enter_long_mode
```

### 设计思考

为什么保护模式不需要做内核相关的工作？

**1. BIOS 中断的限制**

E820 内存枚举和磁盘读取都依赖 BIOS 中断（INT 0x15 和 INT 0x13）。这些中断只能在 Real Mode 调用。进入保护模式后，BIOS 中断就不可用了。

**2. 内存布局的一致性**

Real Mode 已经把内核加载到 0x20000，内存布局已经固定。保护模式不需要重新定位或处理这些数据。

**3. 模式切换的纯粹性**

保护模式只是一个过渡阶段。我们的目标是 Long Mode，保护模式的作用就是完成 CPU 状态的转换，而不是处理业务逻辑。

---

## 第二阶段 —— Real Mode 两件大事的协同

Real Mode 完成的两件大事——E820 和磁盘读取——是精心设计的协作关系。

### 2.1 E820 内存枚举：为后续提供地图

E820 返回的内存地图告诉我们哪些内存区域可用，哪些是保留的。这些信息对内核的内存管理至关重要。

```asm
// file: boot/common/boot.S

// E820 内存布局
.set E820_BUFFER_ADDR,          0x5000
.set E820_BUFFER_COUNT_ADDR,    0x5000      // entry_count 在 0x5000
.set E820_BUFFER_ENTRIES_ADDR,  0x5004      // entries 数组从 0x5004 开始
```

调用 `query_memory_map` 后，0x5000 处的数据结构：

```
0x5000: uint32_t entry_count    // 内存条目数量
0x5004: MemoryMapEntry[0]       // 第一个条目（24 字节）
0x501C: MemoryMapEntry[1]       // 第二个条目
...
```

每个 MemoryMapEntry 包含：
- `base`：物理基地址（8 字节）
- `length`：区域长度（8 字节）
- `type`：内存类型（4 字节，1=可用，2=保留）
- `acpi`：ACPI 扩展属性（4 字节）

### 2.2 磁盘读取：核心任务

磁盘读取是整个引导过程的核心。我们要把 Mini Kernel ELF 从磁盘 LBA=16 读入内存 0x20000。

```asm
// file: boot/common/boot.S

// Mini kernel 配置
.set MINI_KERNEL_LBA,         16          // 磁盘起始 LBA
.set MINI_KERNEL_SECTORS,     832         // 总扇区数 (416KB)
.set MINI_KERNEL_LOAD_PHYS,   0x20000     // 加载物理地址
.set MINI_KERNEL_LOAD_SEG,    0x2000      // 段地址 (0x2000 << 4 = 0x20000)
.set MINI_KERNEL_LOAD_OFF,    0x0000      // 偏移
```

### 2.3 内存布局的精心设计

这个内存布局不是随意选择的，而是经过精心计算：

```
┌─────────────────────────────────────────────────────────────┐
│ 地址范围         │ 内容                            │
├─────────────────────────────────────────────────────────────┤
│ 0x08000          │ stage2 代码                           │
│ 0x09000 ~ 0x19000│ 栈 (Real Mode 和 Protected Mode)     │
│ 0x20000 ~ 0x90000│ Mini Kernel ELF (416KB)               │
├─────────────────────────────────────────────────────────────┤
│ 0x05000          │ E820 内存地图                         │
│ 0x06400          │ VESA framebuffer 信息                │
│ 0x07000          │ BootInfo 结构 (Long Mode 填充)        │
└─────────────────────────────────────────────────────────────┘
```

**关键设计点**：

1. **内核从 0x20000 开始**：避开了栈区域（0x9000 ~ 0x19000）
2. **E820 数据在 0x5000**：低内存固定位置，内核可以方便访问
3. **保护模式栈在 0x90000**：与内核数据完全隔离

---

## 第三阶段 —— 代码详解：E820 内存枚举

让我们拆解 E820 枚举的代码，理解每一步的工作。

### 3.1 函数入口与初始化

```asm
// file: boot/common/boot.S

.global query_memory_map
query_memory_map:
    pushaw                          // 保存所有通用寄存器
    pushw %es
    pushw %ds

    // 初始化 entry_count = 0
    movw $0x0, %ax
    movw $E820_COUNT_SEG, %dx       // DX = 0x0500
    movw %dx, %ds                   // DS = 0x0500
    movw %ax, E820_COUNT_OFF        // [0x5000] = 0
```

**解释**：

- `pushaw` 保存所有 16 位通用寄存器（AX、CX、DX、BX、SP、BP、SI、DI）
- 我们需要访问物理地址 0x5000，所以 DS = 0x0500（实模式地址计算：0x0500 << 4 = 0x5000）

### 3.2 设置缓冲区指针

```asm
    // 设置 ES:DI 指向 entries 数组
    movw $E820_ENTRIES_SEG, %ax     // AX = 0x0500
    movw %ax, %es                   // ES = 0x0500
    movw $E820_ENTRIES_OFF, %di     // DI = 0x0004

    // EBX = 0（首次调用）
    xorl %ebx, %ebx
```

**解释**：

- entries 数组从 0x5004 开始，所以 ES:DI = 0x0500:0x0004
- EBX 是 E820 的"迭代器"，首次调用设为 0

### 3.3 E820 调用循环

```asm
.e820_loop:
    // 检查是否超过最大 entry 数量
    movl E820_COUNT_OFF, %eax       // 读取当前 count
    cmpl $E820_MAX_ENTRIES, %eax    // 与 32 比较
    jae .e820_done                  // 如果 >= 32，退出

    // 准备 E820 调用参数
    movl $E820_SIGNATURE, %edx      // EDX = 'SMAP' (0x534D4150)
    movl $E820_CMD, %eax            // EAX = 0x0000E820
    movl $E820_ENTRY_SIZE, %ecx     // ECX = 24（buffer size）

    int $0x15                       // 调用 BIOS E820

    // ===== 错误检查 =====
    jc .e820_failed                 // CF=1 表示失败
    cmpl $E820_SIGNATURE, %eax       // 验证签名
    jne .e820_failed
    cmpl $20, %ecx                   // BIOS 至少要写 20 字节
    jb .e820_failed
```

**解释**：

- E820 调用需要特定的寄存器设置
- 三重检查：CF 标志、EAX 签名、ECX 字节数
- 每次调用成功后，BIOS 会更新 EBX 作为下一次调用的 continuation value

### 3.4 移动到下一个条目

```asm
    // 移动到下一个 entry
    addl $E820_ENTRY_SIZE, %edi     // DI += 24

    // 增加 entry_count
    movl E820_COUNT_OFF, %eax       // 读取当前 count
    incl %eax                       // count++
    movl %eax, E820_COUNT_OFF       // 写回

    // 检查是否继续迭代
    testl %ebx, %ebx                // EBX=0 表示完成
    jnz .e820_loop                  // 继续循环

.e820_done:
    movl E820_COUNT_OFF, %eax       // 返回 entry count
    movl %eax, %ecx

    popw %ds
    popw %es
    popaw
    ret
```

**解释**：

- 每次成功调用后，DI 前进 24 字节（一个 entry 的大小）
- count 记录总共获取了多少个 entry
- 当 EBX = 0 时，表示 BIOS 已经枚举完所有内存区域

---

## 第四阶段 —— 代码详解：磁盘读取

磁盘读取代码更复杂，因为它需要处理分块读取（BIOS 一次最多读 127 扇区）。

### 4.1 函数入口与初始化

```asm
// file: boot/common/boot.S

.global load_kernel_from_disk
load_kernel_from_disk:
    pusha                           // 保存所有 16 位寄存器
    pushw %es
    pushw %ds

    // 初始化计数器
    xorw %bx, %bx                   // BX = 0（已读扇区数）
    xorw %si, %si                   // SI = 0（缓冲区偏移）

    // 设置 ES 指向 DAP 段
    movw $DAP_SEGMENT, %dx          // DX = 0x07B0
    movw %dx, %es
```

**解释**：

- BX 用于追踪已经读取了多少扇区
- SI 始终为 0，因为我们每次都写入以段边界对齐的地址

### 4.2 主读取循环

```asm
.read_loop:
    // 检查是否读完
    cmpw $MINI_KERNEL_SECTORS, %bx  // 与总数 832 比较
    jae .read_done                  // 如果 BX >= 832，完成

    // 计算本次读取扇区数（min(127, 剩余)）
    movw $MINI_KERNEL_SECTORS, %ax  // AX = 总数
    subw %bx, %ax                   // AX = 剩余数
    cmpw $DISK_MAX_SECTORS_PER_CALL, %ax  // 与 127 比较
    jbe .read_count_ok              // 如果 <= 127，使用 AX
    movw $DISK_MAX_SECTORS_PER_CALL, %ax  // 否则上限为 127

.read_count_ok:
    movw %ax, %bp                   // BP = 本次读取扇区数
```

**解释**：

- 每次最多读 127 扇区（BIOS INT 13h 的限制）
- 剩余不足 127 时，只读剩余部分
- BP 保存本次要读的扇区数（BIOS 调用会破坏 BP，所以需要后续恢复）

### 4.3 构建 DAP（磁盘地址包）

```asm
    // CRITICAL: 重新设置 ES（BIOS 可能破坏它）
    movw $DAP_SEGMENT, %dx          // DX = 0x07B0
    movw %dx, %es

    // 在 0x7B00 处构建 DAP
    movw $DAP_OFFSET, %di           // DI = 0x0000

    // DAP.size = 16
    movb $16, %es:(%di)             // [0x7B00] = 16

    // DAP.reserved = 0
    movb $0, %es:1(%di)             // [0x7B01] = 0

    // DAP.count = BP（本次扇区数）
    movw %bp, %es:2(%di)            // [0x7B02] = 扇区数

    // DAP.buffer 地址计算
    // 目标 = 0x20000 + (BX * 512)
    //      = 0x20000 + (BX << 9)
    // Segment = (0x20000 + (BX << 9)) >> 4
    //         = 0x2000 + (BX << 5)
    movw %bx, %ax                   // AX = 扇区偏移
    shlw $5, %ax                    // AX *= 32 (相当于 >>4 <<9)
    addw $MINI_KERNEL_LOAD_SEG, %ax // AX = 0x2000 + offset
    movw %ax, %es:6(%di)            // buffer segment
    movw $MINI_KERNEL_LOAD_OFF, %ax // AX = 0
    movw %ax, %es:4(%di)            // buffer offset

    // DAP.lba = 16 + BX
    movw $MINI_KERNEL_LBA, %ax      // AX = 16
    addw %bx, %ax                   // AX = 当前 LBA
    movw %ax, %es:8(%di)            // LBA 低 16 位
    xorw %ax, %ax
    movw %ax, %es:10(%di)           // LBA 高 16 位
    movw %ax, %es:12(%di)           // LBA 更高 16 位
    movw %ax, %es:14(%di)           // LBA 最高 16 位（清零）
```

**解释**：

- DAP 结构固定 16 字节，告诉 BIOS 读什么、读多少、写到哪
- 缓冲区地址需要动态计算：每读 512 字节（1 扇区），段地址增加 32（512/16=32）
- LBA 从 16 开始，每次增加已读扇区数

### 4.4 调用 BIOS INT 13h

```asm
    // CRITICAL: 保存 BX 和 BP（BIOS 会破坏它们）
    pushw %bx                       // 保存扇区计数器
    pushw %bp                       // 保存本次扇区数

    // 调用 INT 0x13 AH=0x42
    // 注意：INT 13h AH=42h 需要 DS:SI 指向 DAP，不是 ES:SI
    movw $DAP_SEGMENT, %dx          // DX = 0x07B0
    movw %dx, %ds                   // DS = DAP 段（必须！）
    movw $DAP_OFFSET, %si           // SI = 0x0000
    movb $0x80, %dl                 // DL = 0x80（第一块硬盘）
    movb $DISK_READ_CMD, %ah        // AH = 0x42

    int $0x13                       // 执行磁盘读取

    jc .disk_error_restore_bp       // CF=1 表示失败
    cmpb $0, %ah                    // AH 应该为 0
    jne .disk_error_restore_bp      // AH!=0 表示错误

    // 成功：恢复 BP 和 BX
    popw %bp
    popw %bx

    // 更新扇区计数器
    addw %bp, %bx                   // BX += 本次读取扇区数

    jmp .read_loop                  // 继续下一次读取
```

**解释**：

- **关键坑**：INT 13h AH=0x42 需要 DS:SI 指向 DAP，不是 ES:SI！
- BX 和 BP 必须在调用前保存，BIOS 会破坏它们
- 成功后更新 BX，继续循环直到读完所有扇区

### 4.5 完成与错误处理

```asm
.read_done:
    popw %ds
    popw %es
    popa

    // 验证输出：'O' 表示 OK
    movb $'O', %al
    outb %al, $0xe9

    movw $MINI_KERNEL_SECTORS, %ax  // 返回扇区数
    ret

disk_read_failed:
    // 错误处理
    movb $'F', %al
    outb %al, $0xe9
    popw %ds
    popw %es
    popa
    movw $(msg_disk_read_failed), %si
    jmp panic
```

---

## 第五阶段 —— 保护模式的"静默"

现在我们进入保护模式。`pm_entry` 的代码很简单，但每一步都有其意义。

```asm
// file: boot/stage2.S

.code32                          // Now in 32-bit protected mode
pm_entry:
    // Set up data segment registers
    movw $0x10, %ax               // Data selector value
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    // Set up new stack
    movl $0x90000, %esp           // New stack in protected mode

    // Debug output: 'P' for Protected Mode
    movb $0x50, %al               // 'P'
    outb %al, $0xE9               // Debugcon output

    // ============================================================
    // 004_boot_load_mini_kernel_B: No operation needed
    // ============================================================
    // Mini kernel bin was already loaded to 0x20000 in real mode.
    // Entry point is fixed at 0xFFFFFFFF80020000 (high-half kernel).
    // Nothing to do in protected mode, proceed to long mode.

    // Setup page tables and enter long mode
    call setup_page_tables
    call enter_long_mode
```

### 为什么保护模式不操作内核数据？

**1. 数据已经在正确位置**

Real Mode 已经把内核加载到 0x20000。这个地址在页表映射下，对应的虚拟地址是 `0xFFFFFFFF80020000`（高半内核）。保护模式不需要重新定位。

**2. 段寄存器已经正确设置**

我们只需要把 DS、ES、FS、GS、SS 设置为数据段选择子（0x10），CPU 就能正确访问内存。

**3. 页表映射在 Long Mode 完成**

高半内核映射需要 64 位页表，这只能在 Long Mode 完成。保护模式只是过渡。

---

## 第六阶段 —— Debugcon 验证

虽然你说 debugcon 只是验证手段，但它在调试过程中非常有用。

### 预期输出序列

```
Real Mode:  (可选，用于标记阶段)
Protected Mode: P
Long Mode: L
Disk Read: O (OK) 或 F (Failed)
```

完整的 debug.log 应该包含：

```
Stage2 OK
Mode info OK, switching...
PLO
```

- `P`：成功进入保护模式
- `L`：成功进入 Long Mode
- `O`：磁盘读取成功

### 代码中的调试输出

```asm
// 磁盘读取成功后
movb $'O', %al
outb %al, $0xe9

// 磁盘读取失败
movb $'F', %al
outb %al, $0xe9
```

这些简单的字符输出在调试时非常有效，不需要复杂的串口初始化。

---

## 第七阶段 —— 万事俱备，只欠跳转

当 CPU 进入 Long Mode 后，所有数据都已经准备就绪：

```
┌─────────────────────────────────────────────────────────────┐
│ 数据结构              │ 位置         │ 状态                 │
├─────────────────────────────────────────────────────────────┤
│ MemoryMapEntry[]     │ 0x5000       │ 已填充 (E820)        │
│ Framebuffer info     │ 0x6400       │ 已填充 (VESA)        │
│ BootInfo 结构         │ 0x7000       │ 待填充 (Long Mode)   │
│ Mini Kernel ELF      │ 0x20000      │ 已加载 (磁盘读)      │
└─────────────────────────────────────────────────────────────┘
```

下一步（下一章）的工作：

1. **填充 BootInfo**：把 E820 数据、framebuffer 信息、内核入口点等写入 BootInfo 结构
2. **设置高半内核映射**：确保虚拟地址 `0xFFFFFFFF80020000` 映射到物理 `0x20000`
3. **跳转到内核**：`jmp *0xFFFFFFFF80020000`，控制权移交给内核

这就是"万事俱备，只欠跳转"的含义。所有准备工作都已完成，只剩下最后一步。

---

## 常见问题排查

### 问题 1：磁盘读取出错（看到 F 而不是 O）

可能原因：
1. DAP 地址计算错误：检查 DS:SI 是否正确指向 0x7B00
2. 缓冲区地址错误：确认 segment = 0x2000 + (BX << 5)
3. LBA 错误：确认起始 LBA = 16

排查方法：
- 用 GDB 检查 0x7B00 处的 DAP 内容
- 检查 0x20000 处是否有 ELF 数据（`x/10x 0x20000`，应该看到 `0x7F 'ELF'`）

### 问题 2：看不到 P 输出

可能原因：
1. GDT 加载失败：检查 lgdt 指令前 DS 是否为 0
2. 远跳转失败：确认 `ljmp $0x08, $pm_entry` 的选择子正确
3. 栈设置错误：检查 ESP 是否为 0x90000

### 问题 3：内存冲突导致崩溃

参考之前的笔记 `document/notes/005/kernel_load_stack_collision.md`：

- 确认 `MINI_KERNEL_LOAD_PHYS = 0x20000`（大于栈顶 0x19000）
- 确认内核大小不超过预留空间

---

## 本章踩坑总结

1. **BIOS 调用的寄存器破坏**：INT 13h 和 INT 15h 会破坏多个寄存器，必须提前保存
2. **DAP 需要 DS:SI 而不是 ES:SI**：这是一个文档不清晰的坑，容易搞错
3. **段地址计算公式**：`segment = physical >> 4`，不是直接使用物理地址
4. **实模式栈容易被覆盖**：内核加载地址必须大于栈顶（0x19000）
5. **分块读取的复杂性**：BIOS 一次最多读 127 扇区，需要循环处理
6. **E820 的三重检查**：CF 标志、EAX 签名、ECX 字节数，缺一不可

---

## 收尾

到这里，004B 章的内容就结束了。我们理解了为什么保护模式"什么都不做"，以及 Real Mode 完成的两件大事如何协同工作。

**核心要点**：

1. **Real Mode 完成所有"搬运"工作**：E820 枚举和磁盘读取都在 Real Mode 完成
2. **保护模式只是过渡**：它的作用是完成 CPU 状态转换，不做业务逻辑
3. **内存布局精心设计**：0x20000 的内核地址避开了栈区域，避免冲突
4. **Debugcon 是简单有效的验证手段**：一个字符输出就能确认阶段完成

下一章（004C 或 005），我们将讲解如何在 Long Mode 填充 BootInfo，完成最后的数据准备，然后跳转到内核入口点。那将是 bootloader 的最后一步工作。

---

## 最重要三条认知（必须记住）

**Real Mode 是数据搬运的主力**：所有依赖 BIOS 的操作（E820、磁盘读取）都在 Real Mode 完成。这不是"偷懒"，而是架构设计的必然选择——BIOS 中断只能在 Real Mode 工作。

**保护模式的"静默"是设计的优雅**：它不做额外操作，只负责 CPU 状态转换。这种职责分离让代码更清晰，也避免了在保护模式处理 BIOS 兼容性问题。

**内存布局必须全局考虑**：不能只看"代码在哪里"，必须考虑"栈在哪里"、"数据在哪里"。0x20000 的内核地址不是随意选择的，而是经过精心计算，确保不与栈（0x9000~0x19000）冲突。

---

## 参考资料

### Intel/AMD 手册
- Intel SDM Vol. 3A, Chapter 9: Processor Management and Initialization
- Intel SDM Vol. 3A, Section 9.8: Entering Long Mode
- AMD APM Vol. 2, Chapter 5: Long Mode

### BIOS 中断
- INT 0x15/E820: System Memory Map
- INT 0x13/AH=0x42: Extended Read Sectors From Drive

### OSDev Wiki
- https://wiki.osdev.org/Detecting_Memory_(x86)
- https://wiki.osdev.org/ATA_in_x86_RealMode_(BIOS)
- https://wiki.osdev.org/Entering_Long_Mode

### 项目内部文档
- `document/tutorial/004_boot_load_mini_kernel_A.md`：E820 和磁盘读取的详细实现
- `document/notes/005/kernel_load_stack_collision.md`：内存布局与栈冲突问题
- `document/notes/005/verify-loading.md`：磁盘读取验证方法

---

**文件路径汇总**：

- [boot/common/boot.S](../../boot/common/boot.S) - E820 和磁盘读取实现
- [boot/stage2.S](../../boot/stage2.S) - 保护模式入口和 Long Mode 切换
- [boot/boot_info.h](../../boot/boot_info.h) - BootInfo 结构定义

下一章预告：`004C_fill_bootinfo` 或 `005_jump_to_kernel` —— 在 Long Mode 填充 BootInfo 结构，完成高半内核映射，然后跳转到内核入口点。那将是 bootloader 的谢幕演出。
