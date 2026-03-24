# 004 通读版 · 从磁盘加载小内核

## 章节概览

前几章我们把启动链条搭好了：MBR -> Stage2 -> Protected Mode -> Long Mode。但到上一章为止，stage2 进入 Long Mode 后只是输出了一个 `L` 字符然后 halt，这就有点尴尬了——我们费这么大劲进入 64 位模式，结果什么都没干就停机了？本章的目标非常直接：让 bootloader 真正从磁盘读取一个小内核（mini kernel），确认数据正确加载，然后跳转过去执行。听上去简单？这一步涉及到 E820 内存探测、INT 13h 扩展读、ELF 格式解析等一系列坑，我调试时遇到的 DL 寄存器设置错误导致的读取失败就是个典型例子。

在整个 Cinux OS 的架构中，本章扮演着连接 bootloader 和真正内核的桥梁角色。上一章我们进入了 Long Mode，但没有任何实质性的工作完成。本章要做的事情非常关键：在 Real Mode 阶段完成 E820 内存探测（把内存布局保存到固定地址 0x5000），然后通过 INT 13h AH=0x42 扩展读取指令从磁盘 LBA=16 处读取小内核的 ELF header 到 0x10000。完成这一步后，我们就能确认磁盘读取链路是通的，为后续加载完整的 ELF64 内核打下基础。

本章的核心设计决策包括：在 Real Mode 阶段完成 E820 内存探测（避免进入 Protected Mode 后无法调用 BIOS INT 15h）；使用固定地址 0x5000 保存 E820 结果（简化内核侧的解析逻辑）；采用 DAP（Disk Address Packet）结构进行扩展读取（突破传统 CHS 寻址的 8GB 限制）；将内核读取到 0x10000（而非最终执行地址 0x200000），先验证数据再决定后续处理；使用最简化的 mini kernel（只有 CLI+HLT 循环）专注于验证读取链路。与 GRUB 这种成熟 bootloader 相比，我们的实现更加原始——GRUB 支持多种文件系统、能解析完整的 ELF、能处理多阶段启动，而我们只专注于从固定 LBA 读取固定大小的数据。

### 关键设计决策一览

* **Real Mode 内完成 E820**：在切换到 Protected Mode 之前调用 INT 15h E820，结果保存到 0x5000
* **固定地址约定**：E820 buffer 在 0x5000，DAP 在 0x7B00，kernel 临时加载到 0x10000
* **INT 13h 扩展读**：使用 AH=0x42 配合 DAP 结构，支持 LBA 寻址和大容量磁盘
* **先验证后跳转**：本章只验证数据能正确读取和加载，实际跳转执行留到下一章
* **最小化 mini kernel**：只有 _start 入口和死循环，专注验证读取链路而非内核功能

---

## 架构图

下面是本章涉及的内存布局、磁盘读取流程和调用关系图，帮助你理解各个组件是如何协作的：

```
+---------------------------------------------------------------------+
|                        低内存布局（Real Mode 可用）                   |
+---------------------------------------------------------------------+
|  0x00005000  +----------------------------------------------+       |
|              |  E820 Buffer                                  |       |
|              |  [0x0000] count (u32)                         |       |
|              |  [0x0004] entries[32]                         |       |
|              |    [0] base=0x0 len=0x9FC00 type=1           |       |
|              |    [1] base=0x9FC00 len=0x400 type=2         |       |
|              |    ...                                        |       |
|  0x00007B00  +----------------------------------------------+       |
|              |  DAP (Disk Address Packet)                    |       |
|              |  [0x00] size = 0x10                           |       |
|              |  [0x01] reserved = 0                          |       |
|              |  [0x02] count = 8 sectors                     |       |
|              |  [0x04] buffer_offset = 0x0000                |       |
|              |  [0x06] buffer_segment = 0x1000 (->0x10000)   |       |
|              |  [0x08] lba_low = 16                          |       |
|              |  [0x0C] lba_high = 0                          |       |
|  0x00008000  +----------------------------------------------+       |
|              |  Stage2 Bootloader                            |       |
|  0x00010000  +----------------------------------------------+       |
|              |  Mini Kernel ELF Header (临时加载位置)         |       |
|              |  [0x00] 0x7F 0x45 0x4C 0x46  ('ELF')          |       |
|              |  [0x10] entry point = 0x200000                |       |
|              |  [0x18] program headers...                    |       |
+---------------------------------------------------------------------+
|                        E820 调用流程                                 |
+---------------------------------------------------------------------+
|                                                                     |
|  Real Mode (stage2)                                                  |
|       |                                                              |
|       v                                                              |
|  +--------------------------+                                       |
|  | call query_memory_map    |  boot/common/boot.S                  |
|  |  EBX=0 (首次调用)         |                                       |
|  |  EDX='SMAP' (0x534D4150)  |                                       |
|  |  ECX=24 (buffer size)     |                                       |
|  |  ES:DI=0x5000:0x0004      |                                       |
|  +--------------------------+                                       |
|       |                                                              |
|       v                                                              |
|  +--------------------------+                                       |
|  | INT 0x15, EAX=0xE820     |  BIOS 调用                            |
|  |  -> CF=0 成功             |                                       |
|  |  -> EAX='SMAP'            |                                       |
|  |  -> ECX=实际条目大小       |                                       |
|  |  -> ES:DI 填入条目数据     |                                       |
|  +--------------------------+                                       |
|       |                                                              |
|       v                                                              |
|  +--------------------------+                                       |
|  | 检查 EBX!=0?              |  EBX=continuation value             |
|  |  是 -> 继续循环            |                                       |
|  |  否 -> 保存count，返回      |  CX=条目数量                        |
|  +--------------------------+                                       |
|                                                                     |
+---------------------------------------------------------------------+
|                        INT 13h 扩展读流程                             |
+---------------------------------------------------------------------+
|                                                                     |
|  Real Mode (stage2)                                                  |
|       |                                                              |
|       v                                                              |
|  +--------------------------+                                       |
|  | call load_kernel_from_disk|  boot/common/boot.S                  |
|  |  1. 构造 DAP @ 0x7B00     |                                       |
|  |  2. 设置 DS:SI = DAP 地址  |                                       |
|  |  3. DL = 0x80 (硬盘)       |                                       |
|  |  4. AH = 0x42 (扩展读)     |                                       |
|  +--------------------------+                                       |
|       |                                                              |
|       v                                                              |
|  +--------------------------+                                       |
|  | INT 0x13, AH=0x42        |  BIOS 磁盘读取                         |
|  |  -> CF=0 成功             |                                       |
|  |  -> AH=0 无错误           |                                       |
|  |  -> AL=读取的扇区数        |                                       |
|  +--------------------------+                                       |
|       |                                                              |
|       v                                                              |
|  数据已加载到 0x10000                                                |
|  验证: [0x10000] = 0x7F ('ELF' magic)                               |
|                                                                     |
+---------------------------------------------------------------------+
|                        Stage2 调用时机                               |
+---------------------------------------------------------------------+
|                                                                     |
|  _start (stage2.S)                                                   |
|       |                                                              |
|       v                                                              |
|  print "Stage2 OK"                                                   |
|       |                                                              |
|       v                                                              |
|  enable_a20                                                          |
|       |                                                              |
|       v                                                              |
|  vesa_get_controller_info                                            |
|  vesa_get_mode_info                                                  |
|  vesa_set_mode                                                       |
|  vesa_save_framebuffer_info                                          |
|       |                                                              |
|       v                                                              |
|  +--------------------------+  <-- 004_boot_load_mini_kernel_A       |
|  | call query_memory_map    |  在 Real Mode 内完成                  |
|  +--------------------------+                                       |
|       |                                                              |
|       v                                                              |
|  +--------------------------+                                       |
|  | call load_kernel_from_disk|  在 Real Mode 内完成                  |
|  +--------------------------+                                       |
|       |                                                              |
|       v                                                              |
|  cli (禁用中断)                                                      |
|       |                                                              |
|       v                                                              |
|  Switch to Protected Mode                                            |
|  Setup Long Mode                                                     |
|  Halt at long_mode_entry                                             |
|                                                                     |
+---------------------------------------------------------------------+
|                        磁盘布局约定                                  |
+---------------------------------------------------------------------+
|                                                                     |
|  LBA 0:       MBR (1 sector, 0x7C00)                                 |
|  LBA 1-15:    Stage2 (15 sectors max, 0x8000)                       |
|  LBA 16-23:   Mini Kernel ELF (8 sectors = 4KB, 0x10000)            |
|  LBA 24+:     (后续预留)                                             |
|                                                                     |
+---------------------------------------------------------------------+
```

---

## 关键代码精讲

接下来我们要逐段拆解本章的核心代码。本章的新增逻辑主要位于 `boot/common/boot.S`，这个文件封装了两个关键函数：`query_memory_map` 执行 E820 内存探测，`load_kernel_from_disk` 从磁盘读取内核。stage2.S 在适当的位置调用这些函数，在进入 Protected Mode 之前完成 Real Mode 的最后一项工作。

### 常量定义：与硬件约定的魔法数字

代码开头定义了一系列与 BIOS 调用相关的常量，这些数字背后都有其硬件层面的含义：

```asm
// E820 memory layout
.set E820_BUFFER_ADDR,          0x5000
.set E820_BUFFER_COUNT_ADDR,    0x5000
.set E820_BUFFER_ENTRIES_ADDR,  0x5004
.set E820_MAX_ENTRIES,          32
.set E820_ENTRY_SIZE,           24

// Pre-calculated segment/offset values
.set E820_COUNT_SEG,            0x0500      // 0x0500 << 4 = 0x5000
.set E820_COUNT_OFF,            0x0000
.set E820_ENTRIES_SEG,          0x0500
.set E820_ENTRIES_OFF,          0x0004
```

这里有一个关键细节：Real Mode 的地址计算方式是 `物理地址 = 段寄存器 << 4 + 偏移`。所以要访问物理地址 0x5000，我们需要把段寄存器设置为 0x0500（0x0500 << 4 = 0x5000），偏移为 0。我在调试时踩过一个坑：直接把段寄存器设置为 0x5000，结果访问的是 0x50000，完全错位了。这就是为什么代码中定义了 `E820_COUNT_SEG = 0x0500` 而不是 `0x5000`。

E820 条目大小为 24 字节，对应 C 结构体：

```c
struct E820Entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi_attrs;  // 可选，BIOS 可能只返回 20 字节
};
```

我们要求 BIOS 返回 24 字节，但实际检测时如果 ECX < 20 就认为失败，这是因为 BIOS 可能不返回 ACPI 扩展属性。

```asm
// DAP (Disk Address Packet) structure offsets (16 bytes)
.set DAP_SIZE,                0
.set DAP_RESERVED,            1
.set DAP_COUNT,               2
.set DAP_BUFFER_OFFSET,       4
.set DAP_BUFFER_SEGMENT,      6
.set DAP_LBA,                 8

// DAP fixed location
.set DAP_PHYS_ADDR,           0x7B00
.set DAP_SEGMENT,             0x07B0      // 0x07B0 << 4 = 0x7B00
.set DAP_OFFSET,              0x0000

// Disk read constants
.set MINI_KERNEL_LBA,         16
.set KERNEL_LOAD_SEGMENT,     0x1000      // 0x10000 = 0x1000:0x0000
.set DISK_READ_CMD,           0x42
```

DAP 结构用于 INT 13h AH=0x42 扩展读取。传统的 CHS（磁头/柱面/扇区）寻址方式最多只能访问 8GB 磁盘，而且需要知道磁盘的物理几何结构。LBA（逻辑块地址）方式直接用线性编号，更简单也支持更大容量。我们选择 LBA=16 作为小内核的起始位置，这是因为 LBA 0 是 MBR（1 扇区），LBA 1-15 是 Stage2（最多 15 扇区），LBA 16 开始正好空出来给内核用。

### E820 内存探测：询问 BIOS "内存长什么样"

`query_memory_map` 函数负责枚举系统内存布局，这是操作系统物理内存管理的基础：

```asm
.global query_memory_map
query_memory_map:
    pushaw
    pushw %es
    pushw %ds

    // 初始化 count = 0
    movw $0x0, %ax
    movw $E820_COUNT_SEG, %dx
    movw %dx, %ds
    movw %ax, E820_COUNT_OFF     // count = 0 @ 0x5000

    // 设置 ES:DI 指向 entries 数组
    movw $E820_ENTRIES_SEG, %ax
    movw %ax, %es
    movw $E820_ENTRIES_OFF, %di

    // EBX = 0 表示首次调用
    xorl %ebx, %ebx
```

E820 是一个迭代式的 BIOS 调用。第一次调用时 EBX=0，BIOS 返回第一条内存条目和 continuation value（放在 EBX 中）。后续调用用上次的 EBX 值，直到 EBX=0 表示枚举完成。这种设计支持 BIOS 管理任意数量的内存条目，不需要预先知道数量。

```asm
.e820_loop:
    // 检查是否超过最大条目数
    movl E820_COUNT_OFF, %eax
    cmpl $E820_MAX_ENTRIES, %eax
    jae .e820_done

    // 设置 E820 调用参数
    movl $E820_SIGNATURE, %edx       // EDX = 'SMAP'
    movl $E820_CMD, %eax             // EAX = 0x0000E820
    movl $E820_ENTRY_SIZE, %ecx      // ECX = 24 (buffer size)

    int $0x15
    jc .e820_failed                  // CF=1 表示错误

    cmpl $E820_SIGNATURE, %eax       // 验证签名
    jne .e820_failed

    cmpl $20, %ecx                   // BIOS 必须返回至少 20 字节
    jb .e820_failed

    // 移动到下一个条目位置
    addl $E820_ENTRY_SIZE, %edi

    // 递增 count
    movl E820_COUNT_OFF, %eax
    incl %eax
    movl %eax, E820_COUNT_OFF

    // 检查是否继续
    testl %ebx, %ebx                // EBX=0 表示完成
    jnz .e820_loop
```

这里有几个关键点需要特别注意。首先，EAX 必须是完整的 `0x0000E820`，不能只用 `0xE820`。我在调试时发现有些 BIOS 对这个值很敏感，写错就会返回错误。其次，ECX=24 表示我们期望 BIOS 返回 24 字节的条目，但 BIOS 可能只返回 20 字节（不带 ACPI 扩展），所以检查时用 `cmpl $20, %ecx` 而不是 24。第三，每次调用后 DI 会自动加上 ECX 返回的实际条目大小，但我们这里手动加 24，这是为了确保条目之间紧密排列，便于内核侧解析。

E820 条目的 type 字段表示内存类型：

* Type 1: 可用内存（Usable）
* Type 2: 保留内存（Reserved）
* Type 3: ACPI Reclaimable
* Type 4: ACPI NVS
* 其他: 各种保留或特殊用途

我们的内核只会使用 Type=1 的内存区域，其他区域都必须避开。在我调试时遇到的坑是：虽然 E820 调用成功返回了 7 条记录，但最初解析时没有正确过滤 type，导致把 BIOS 保留区也当可用内存用了，后果就是各种莫名其妙的崩溃。

### INT 13h 扩展读取：从磁盘搬运数据

`load_kernel_from_disk` 函数负责从磁盘读取内核 ELF 到内存：

```asm
.global load_kernel_from_disk
load_kernel_from_disk:
    pushaw
    pushw %es
    pushw %ds

    // 设置 ES 指向 DAP 段
    movw $DAP_SEGMENT, %ax           // ES = 0x07B0
    movw %ax, %es

    // DI 指向 DAP 开始
    movw $DAP_OFFSET, %di

    // 填充 DAP.size = 16
    movb $16, %es:(%di)

    // 填充 DAP.count = 8 (4KB = 8 sectors)
    movw $8, %es:2(%di)

    // 填充 DAP.buffer = 0x10000
    movw $0x0000, %es:4(%di)         // offset
    movw $0x1000, %es:6(%di)         // segment = 0x1000
```

DAP 结构的构造过程需要仔细处理每个字段。size 字段必须是 16（DAP 结构的大小），count 表示要读取的扇区数。我们选择 8 个扇区（4KB）作为本次读取的量，这足以容纳 ELF header 和程序头表。buffer 字段用 segment:offset 格式表示，0x1000:0x0000 对应物理地址 0x10000。

```asm
    // 填充 DAP.lba = 16
    movl $MINI_KERNEL_LBA, %eax      // EAX = 16
    movl %eax, %es:8(%di)            // LBA low 32 bits
    xorl %eax, %eax
    movl %eax, %es:12(%di)           // LBA high 32 bits

    // 设置 DS:SI 指向 DAP
    movw $DAP_SEGMENT, %dx
    movw %dx, %ds
    movw $DAP_OFFSET, %si

    // 关键：DL 必须是 0x80（第一块硬盘）
    movb $0x80, %dl
    movb $DISK_READ_CMD, %ah         // AH = 0x42
    int $0x13
```

这里有一个我调试时踩过的经典坑：DL 寄存器必须设置为 0x80 才能访问第一块硬盘。DL 的编码方式是：0x00-0x7F 是软盘驱动器，0x80-0xFF 是硬盘驱动器。如果 DL 设置错误（比如我最初忘了设置，DL 可能是 0x01），BIOS 会返回 AH=0x01（Invalid function）或 AH=0x80（No media），读取会失败。更坑的是，有时候 QEMU 的 BIOS 比较宽容，即使 DL 错误也能返回成功，但读到的是垃圾数据，这种情况下排查起来就更难了。

INT 13h AH=0x42 的成功判断标准是：CF=0 且 AH=0。CF（Carry Flag）是 BIOS 调用的通用错误标志，任何错误都会置位。AH 存放扩展错误码，0 表示成功。如果 CF=0 但 AH!=0，这表示"操作完成但有问题"，需要检查 AH 的具体值。

```asm
    // 检查错误
    jc disk_read_failed
    cmpb $0, %ah
    jne disk_read_failed

    // 成功：返回扇区数
    popw %ds
    popw %es
    popaw
    movw $8, %ax                    // 返回 8 sectors
    ret
```

### Stage2 调用时机：Real Mode 的最后一舞

stage2.S 中的调用顺序非常关键。两个新函数必须在进入 Protected Mode 之前调用：

```asm
    // VESA 操作（仍在 Real Mode）
    call vesa_get_controller_info
    call vesa_get_mode_info
    call vesa_set_mode
    call vesa_save_framebuffer_info

    // ============================================================
    // 004_boot_load_mini_kernel_A: Real Mode 内完成
    // ============================================================

    call query_memory_map           // [->0x5000] E820 memory map
    call load_kernel_from_disk      // [->0x10000] Load mini kernel ELF header

    cli                             // 再次禁用中断

    // ============================================================
    // Switch to Protected Mode
    // ============================================================
```

为什么必须在 Real Mode 调用这两个函数？因为它们依赖 BIOS 中断。一旦进入 Protected Mode，我们就不能直接调用 BIOS 中断了（BIOS 代码是 16 位 Real Mode 代码，保护模式下无法执行）。E820 需要调用 INT 15h，磁盘读取需要调用 INT 13h，这些都只能在 Real Mode 完成。

另一个细节是 `cli` 指令的位置。我们在调用 VESA 和新函数期间是开启中断的（之前的 `sti`），但在切换到 Protected Mode 之前再次禁用中断。这是因为模式切换期间必须确保中断被禁用，否则如果在中断处理程序中发生了模式切换，后果不堪设想。

### Mini Kernel：极简的验证目标

`kernel/mini/main.cpp` 是本章的目标"内核"，它极其简单：

```cpp
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

这个 mini kernel 只做一件事：无限循环执行 CLI+HLT。CLI 禁用中断，HLT 让 CPU 停机等待中断。因为没有中断，所以 CPU 会永远停在这里。这正好用于验证：如果 bootloader 成功跳转到这里，QEMU 会正常停机而不是崩溃。

但等等——mini kernel 的链接器脚本指定了不同的加载地址：

```ld
SECTIONS
{
    /* Kernel loads at physical 2MB */
    . = 0x200000;

    .text : { *(.text) }
    ...
}
```

这里有个有趣的设计：linker.ld 指定内核的执行地址是 0x200000，但 bootloader 只读到 0x10000。这意味着本章我们只验证"能读取"，不验证"能执行"。真正的重定位和跳转留到下一章处理。这种分阶段验证的好处是可以逐步排查问题——如果读取都失败了，跳转就更不用说了。

### 构建系统集成

CMakeLists.txt 的变化也值得关注。boot/CMakeLists.txt 现在把 boot.S 编译进 boot_common 目标：

```cmake
add_library(boot_common OBJECT
    common/serial.S
    common/boot.S      # 新增
)
```

而 kernel/CMakeLists.txt 添加了 mini 子目录：

```cmake
add_subdirectory(mini)
```

kernel/mini/CMakeLists.txt 定义了 mini_kernel 的编译规则：

```cmake
target_compile_options(mini_kernel PRIVATE
    -ffreestanding
    -fno-exceptions
    -fno-rtti
    -fno-pie
    -mcmodel=large           # 关键：支持任意地址加载
    -mno-red-zone
)
```

`-mcmodel=large` 这个选项很重要。它告诉 GCC 生成的代码可以加载到任意 64 位地址，而不是假设代码在某个特定范围内（如 `-mcmodel=kernel` 假设在 -2GB 到 +2GB 之间）。我们的 mini kernel 最终会链接到 0x200000，但临时加载到 0x10000，所以需要 large model 来保证代码能正确运行（如果真的执行的话）。

---

## 设计决策深度分析

### 决策 1：在 Real Mode 完成 E820 vs 延迟到保护模式

**问题**：E820 内存探测必须在 Real Mode 调用 BIOS INT 15h，但结果可以等到进入保护模式后再解析。问题是何时调用、何时解析？

**本项目的做法**：我们在 Real Mode 的最后阶段（进入 Protected Mode 之前）调用 E820，结果直接保存到固定地址 0x5000。Bootloader 不做任何解析，只是"搬运工"角色，把 BIOS 返回的原始数据完整保存。后续内核代码会解析这个 buffer，构建自己的物理内存管理器。

**备选方案**：另一种设计是在 bootloader 阶段就解析 E820，提取可用内存区域，构建一个更紧凑的数据结构传给内核。或者完全跳过 E820，硬编码 QEMU 的默认内存布局（比如假设有 512MB RAM 从 0x0 开始）。

**为什么不选备选方案**：在 bootloader 解析会增加代码复杂度，而且 bootloader 不知道内核需要什么格式的数据。直接传递原始 E820 数据是最灵活的——内核可以按需解析，甚至可以忽略某些条目。硬编码内存布局虽然简单，但会完全丧失移植性，一旦换个 QEMU 配置或真实硬件就会崩溃。

**如果要扩展/改进**：当前实现假设 E820 buffer 最多 32 条记录。对于某些特殊配置（如大量 MMIO 区域），可能需要更多条目。可以改进为动态分配 buffer，或者在 boot_common 中实现一个压缩算法，合并相邻的相同类型条目。另一个改进方向是添加"可用内存总量"统计，在 0x5000 前面加一个 summary 字段，方便内核快速判断。

### 决策 2：INT 13h 扩展读 vs 传统 CHS 读取

**问题**：从磁盘读取数据有两种 BIOS 调用方式：INT 13h AH=0x02（传统 CHS 模式）和 AH=0x42（扩展 LBA 模式）。选择哪种方式会影响代码复杂度和兼容性。

**本项目的做法**：我们选择了 AH=0x42 扩展读取，使用 DAP（Disk Address Packet）结构指定 LBA 地址和缓冲区。这种方式支持 LBA 寻址，不依赖磁盘几何结构，而且理论上支持超过 8GB 的大容量磁盘。

**备选方案**：传统的 AH=0x02 方式需要指定磁头、柱面、扇区号（CHS），这种寻址方式受限于古老的磁盘几何结构，而且大多数现代 BIOS 的 CHS-to-LBA 转换可能有 bug。更糟的是，CHS 只能访问 8GB 以下的空间（1024 柱面 × 255 磁头 × 63 扇区 × 512 字节 ≈ 8.4GB）。

**为什么不选备选方案**：CHS 是上个世纪的产物了，现在还在用纯属"考古"。LBA 是现代标准，所有 BIOS 和磁盘控制器都支持。而且 DAP 结构虽然比 CHS 参数复杂一点，但更直观——直接指定"从第几个扇区读几个扇区到哪个地址"，不需要考虑磁头和柱面。

**如果要扩展/改进**：当前实现固定读取 8 个扇区（4KB）。对于较大的内核，需要分多次读取或增加 count 值。可以改进为"先读取 ELF header，解析程序头表，然后按需加载每个 segment"。这种"按需加载"策略可以减少不必要的磁盘 I/O，但会增加 bootloader 的复杂度。另一个改进是添加错误重试机制，如果第一次读取失败，等待一小段时间后重试，这对某些慢速设备可能有帮助。

### 决策 3：固定地址约定 vs 动态发现

**问题**：Bootloader 和内核之间需要传递数据（E820、framebuffer info、kernel image等）。这些数据应该放在哪里？是固定地址约定还是动态发现？

**本项目的做法**：我们采用固定地址约定的方式：
- E820 buffer: 0x5000
- DAP: 0x7B00
- Framebuffer info: 0x6400
- Kernel 临时加载: 0x10000

这些地址都是硬编码在 bootloader 和内核双方的，不需要任何"发现"机制。内核代码直接从这些地址读取数据，Bootloader 直接写到这些地址。

**备选方案**：另一种设计是使用一个"BootInfo"结构体，放在固定地址（如 0x7000），里面包含各种数据的指针。或者实现一个更复杂的协议，Bootloader 在某个已知地址放一个指针数组，内核按索引查找。GRUB 使用的 multiboot 协议就是这种方式，它定义了一个复杂的 info 结构。

**为什么不选备选方案**：我们的项目规模还很小，固定地址直接访问是最简单的。BootInfo 结构体虽然更优雅，但需要定义结构体布局、处理版本兼容性，增加了维护成本。而且固定地址访问的速度更快——不需要间接指针寻址。

**如果要扩展/改进**：如果未来要支持多种配置或第三方 bootloader，可以考虑实现类似 multiboot 的协议。定义一个标准化的 BootInfo 结构，包含 magic number、版本号、flags，然后是各种数据块的指针和大小。这样内核可以在启动时验证 magic number，确认是正确的 bootloader 加载的。另一个改进方向是添加"数据校验"，在关键数据块后加 CRC32 或 checksum，内核加载时验证，防止数据损坏。

---

## 常见变体与扩展方向

下面列出几个你可以尝试的扩展实验，按照难度排序：

1. **⭐ 添加更多调试输出**：在 E820 循环中输出每个条目的 type，用 debugcon 输出字符表示（如 '1'=可用, '2'=保留）。这样可以在 QEMU 启动时直接看到内存布局，不用 attach GDB。

2. **⭐ 增加 kernel 大小检测**：当前实现假设 mini kernel 正好 8 扇区。你可以修改为：先读取 ELF header，解析 e_shoff（section header offset）或 e_phoff（program header offset），计算实际需要读取的大小，然后分多次读取。

3. **⭐⭐ 实现 CRC32 校验**：在 mini kernel 编译时生成 CRC32 checksum，写入 ELF 的某个 section（如 .note.checksum）。Bootloader 读取后验证 checksum，确保数据完整。如果校验失败，显示错误信息并 halt。

4. **⭐⭐ 支持多个 kernel 版本**：在磁盘上预留多个 LBA 区域（如 LBA 16-31 是 kernel A，LBA 32-47 是 kernel B），Bootloader 根据某个"启动标志"选择加载哪个版本。这可以实现简单的 A/B 更新机制。

5. **⭐⭐⭐ 实现简单的文件系统**：放弃固定 LBA 的方式，实现一个极简的文件系统（如 FAT12 的子集），Bootloader 按文件名查找并读取内核。这需要解析 FAT 表、目录结构等，但能支持动态文件管理。

---

## 参考资料

### Intel/AMD 手册

* **Intel SDM Vol. 3A, Section 15.3.1**: INT 15h, EAX=E820h - Query System Address Map —— E820 调用的官方规范
* **Intel SDM Vol. 2, Chapter 5**: Interrupt and Exception Handling —— 中断调用机制详解
* **BIOS Boot Specification (BBS)**: INT 13h AH=42h - Extended Read —— 磁盘扩展读取的接口定义
* **ELF Specification (Tool Interface Standard)**: ELF64 格式官方规范 —— 了解 ELF header 和 program header 结构

### OSDev Wiki

* [Detecting Memory (x86)](https://wiki.osdev.org/Detecting_Memory_(x86)) —— E820 和其他内存探测方法
* [INT 13h (HDD)](https://wiki.osdev.org/INT_13h) —— 磁盘 I/O 的完整参考
* [ELF](https://wiki.osdev.org/ELF) —— ELF 格式在 OS 开发中的应用
* [Bootloader Tutorial](https://wiki.osdev.org/Bootloader_Tutorial) —— 一个完整的 bootloader 示例

### 其他资源

* [The Little OS Book](https://littleosbook.github.io/) —— 一本免费的小型 OS 开发教程
* [Broken Thumb OS](https://www.brokenthorn.com/Resources/OSDev9.html) —- 详细的 bootloader 开发指南
* [Writing a Simple Operating System from Scratch](https://www.cs.bham.ac.uk/~exr/lectures/opsys/10_11/lectures/os-dev.pdf) —- Nick Blundell 的 OS 开发讲义

---

到这里就大功告成了。如果你跟着教程走下来，现在应该对 Real Mode 的 BIOS 调用、E820 内存探测、INT 13h 磁盘读取有了比较深入的理解。这一步踩过很多坑——段寄存器计算错误、DL 忘记设置 0x80、E820 条目大小不一致——但每一步都是宝贵的学习经验。

下一章我们将做真正有趣的事情：解析刚读取的 ELF header，重定位内核到正确的地址（0x200000），设置好栈和参数，然后跳转到内核的 _start 入口点。到那个时候，我们就能看到 mini kernel 的 CLI+HLT 循环生效，QEMU 正常停机而不是崩溃。但这还只是开始——再往后，我们会在内核里实现串口输出、VESA 文字模式、中断处理，真正的 OS 功能才会逐步展开。敬请期待。
