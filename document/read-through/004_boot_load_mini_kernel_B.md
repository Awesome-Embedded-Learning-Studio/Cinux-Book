# 004 通读版 · Real Mode 加载完成，跳转小内核

## 章节概览

上一章我们在 Real Mode 完成了两件大事：E820 内存探测和读取内核 ELF header 到 0x10000。但那只是"能读取"，本章我们要完成真正关键的一步——确认数据正确加载，然后跳转到小内核执行。听上去只是加个跳转指令？实际上这一步涉及到一系列设计决策：小内核到底应该加载到哪个物理地址？链接地址如何与实际加载地址匹配？BootInfo 结构体如何传递给内核？最关键的是，我们如何确认跳转真的成功了？

在整个 Cinux OS 的启动链条中，本章是 bootloader 向内核移交控制权的"临门一脚"。上一章我们读取了 8 个扇区的 ELF header，但那只是为了验证读取链路。本章要做的更加实际：把完整的小内核（416KB = 832 扇区）从磁盘 LBA=16 加载到 0x20000，确保这个二进制镜像确实是有效的 ELF，然后设置好参数，通过 `jmp *%rax` 跳转到内核入口点。验证手段非常直接：在跳转前用 `outb $0x4A, $0xE9` 输出字符 'J'，如果内核成功接管，QEMU 会在 debugcon 里显示 'J' 然后是内核输出的 'M'（mini kernel 的第一个字符）。

本章的核心设计决策包括：把小内核完整加载到 0x20000（而不是上一章的 0x10000），避开 Real Mode 和 Protected Mode 的栈区；定义 BootInfo 结构体规范 bootloader 和内核之间的数据传递格式；在 Real Mode 完成全部磁盘读取（避免 Protected Mode 无法调用 BIOS）；使用 flat binary 格式（objcopy -O binary）简化内核加载逻辑。与上一章相比，本章从"验证读取链路"升级到"完整加载+跳转"，是 bootloader 功能完整性的一大步。

### 关键设计决策一览

* **完整内核加载**：从 8 扇区增加到 832 扇区（416KB），覆盖完整的小内核二进制
* **地址调整**：加载地址从 0x10000 改为 0x20000，避开栈区和 bootloader 代码区
* **BootInfo 规范**：定义 `boot/boot_info.h`，统一 bootloader 和内核的数据传递格式
* **Real Mode 读取**：全部磁盘 I/O 在进入 Protected Mode 之前完成
* **验证输出**：跳转前输出 'J'（Jump），内核接管后输出 'M'（Mini kernel），形成启动链路证明

---

## 架构图

下面是本章的内存布局、BootInfo 结构和跳转流程图：

```
+---------------------------------------------------------------------+
|                        低内存布局（更新版）                           |
+---------------------------------------------------------------------+
|  0x00005000  +----------------------------------------------+       |
|              |  E820 Buffer (004A)                          |       |
|  0x00006400  +----------------------------------------------+       |
|              |  VESA Framebuffer Info (003)                 |       |
|  0x00007000  +----------------------------------------------+       |
|              |  BootInfo Structure (新增)                   |       |
|              |  [0x00] entry_point (u64)                    |       |
|              |  [0x08] kernel_phys_base (u64)               |       |
|              |  [0x10] kernel_size (u64)                    |       |
|              |  [0x18] fb_addr (u64)                        |       |
|              |  [0x20] fb_width, fb_height, fb_pitch...     |       |
|              |  [0x30] mmap_count (u32)                     |       |
|              |  [0x34] mmap[32] (MemoryMapEntry)            |       |
|  0x00007B00  +----------------------------------------------+       |
|              |  DAP (Disk Address Packet)                   |       |
|  0x00008000  +----------------------------------------------+       |
|              |  Stage2 Bootloader                           |       |
|  0x00009000  +----------------------------------------------+       |
|              |  Protected Mode Stack Bottom                 |       |
|  0x00010000  +----------------------------------------------+       |
|              |  (004A 的加载地址，已废弃)                    |       |
|  0x00020000  +----------------------------------------------+       |
|              |  Mini Kernel Binary (flat binary)            |       |
|              |  [0x20000] _start entry point                |       |
|              |  [0x2000X] .text section                    |       |
|              |  ...                                         |       |
|              |  [0x8XXXX] .bss, stack                      |       |
+---------------------------------------------------------------------+
|                        磁盘布局（更新）                               |
+---------------------------------------------------------------------+
|                                                                     |
|  LBA 0:       MBR (1 sector)                                         |
|  LBA 1-15:    Stage2 (15 sectors)                                    |
|  LBA 16-847:  Mini Kernel Binary (832 sectors = 416KB)              |
|  LBA 848+:    (预留)                                                  |
|                                                                     |
+---------------------------------------------------------------------+
|                        启动流程更新                                  |
+---------------------------------------------------------------------+
|                                                                     |
|  Real Mode (stage2)                                                  |
|       |                                                              |
|       v                                                              |
|  VESA operations (003)                                               |
|       |                                                              |
|       v                                                              |
|  +--------------------------+                                       |
|  | call query_memory_map    |  -> 0x5000 (004A)                    |
|  +--------------------------+                                       |
|       |                                                              |
|       v                                                              |
|  +--------------------------+                                       |
|  | call load_kernel_from_disk|  -> 0x20000 (004B: 832 sectors)     |
|  |    读取完整 416KB         |                                       |
|  +--------------------------+                                       |
|       |                                                              |
|       v                                                              |
|  Switch to Protected Mode                                            |
|       |                                                              |
|       v                                                              |
|  Setup Long Mode                                                      |
|       |                                                              |
|       v                                                              |
|  +--------------------------+                                       |
|  | long_mode_entry          |  64-bit 模式                          |
|  |   movb $'J', %al         |  输出 'J' (即将跳转)                  |
|  |   outb %al, $0xE9        |                                       |
|  +--------------------------+                                       |
|       |                                                              |
|       v                                                              |
|  +--------------------------+                                       |
|  | movabs $0x20000, %rax    |  计算跳转目标                         |
|  | jmp *%rax                |  间接跳转到内核                       |
|  +--------------------------+                                       |
|       |                                                              |
|       v  (跳转成功)                                                   |
|  Mini Kernel _start (0x20000)                                        |
|       |                                                              |
|       v                                                              |
|  +--------------------------+                                       |
|  | debugcon_putc('M')       |  输出 'M' (内核接管)                  |
|  +--------------------------+                                       |
|       |                                                              |
|       v                                                              |
|  cli; hlt (死循环)                                                    |
|                                                                     |
+---------------------------------------------------------------------+
|                        BootInfo 结构体详细                            |
+---------------------------------------------------------------------+
|                                                                     |
|  typedef struct {                                                    |
|      // Kernel information (24 bytes)                               |
|      uint64_t entry_point;        // 0x20000 (flat binary 入口)      |
|      uint64_t kernel_phys_base;   // 0x20000                         |
|      uint64_t kernel_size;        // 实际大小（字节）                |
|                                                                     |
|      // Framebuffer (20 bytes)                                      |
|      uint64_t fb_addr;            // VESA 物理地址                   |
|      uint32_t fb_width;           // 1024                            |
|      uint32_t fb_height;          // 768                             |
|      uint32_t fb_pitch;           // 每行字节数                      |
|      uint32_t fb_bpp;             // 32                              |
|                                                                     |
|      // Memory map (8 + 32*24 = 776 bytes)                          |
|      uint32_t mmap_count;        // E820 条目数                     |
|      uint32_t _pad;              // 对齐填充                        |
|      MemoryMapEntry mmap[32];    // E820 数据 (from 0x5000)         |
|  } BootInfo;  // 总计约 820 字节                                      |
|                                                                     |
+---------------------------------------------------------------------+
```

---

## 关键代码精讲

本章涉及的文件比较多，我们按照逻辑顺序逐个拆解。首先是最核心的 BootInfo 结构体定义，然后是 Real Mode 的磁盘加载逻辑，最后是 Long Mode 的跳转和小内核入口。

### BootInfo 结构体：bootloader 与内核的握手协议

`boot/boot_info.h` 是本章新增的核心文件，它定义了 bootloader 向内核传递信息的统一格式：

```c
#ifndef BOOT_BOOT_INFO_H
#define BOOT_BOOT_INFO_H

#include <stdint.h>

// Memory Map Entry (from E820 BIOS call)
typedef struct {
    uint64_t base;          // Physical base address
    uint64_t length;        // Region length in bytes
    uint32_t type;          // Memory type (1=usable, 2=reserved, etc.)
    uint32_t acpi;          // ACPI extended attributes
} __attribute__((packed)) MemoryMapEntry;

static_assert(sizeof(MemoryMapEntry) == 24, "MemoryMapEntry must be 24 bytes");
```

这里有几个关键设计点需要理解。首先，`__attribute__((packed))` 是必须的——它告诉 GCC 不要在结构体中插入填充字节。E820 返回的数据格式是固定的 24 字节（base=8, length=8, type=4, acpi=4），如果编译器插入填充就会导致错位。`static_assert` 在编译时验证结构体大小，如果因为某种原因（比如不同的编译器对齐策略）大小不对，编译会直接失败，避免运行时的诡异 bug。

```c
// Boot Information Structure
typedef struct {
    // Kernel information
    uint64_t entry_point;       // Virtual entry point address
    uint64_t kernel_phys_base;  // Physical base where kernel loaded
    uint64_t kernel_size;       // Actual ELF file size

    // Framebuffer information
    uint64_t fb_addr;           // Physical framebuffer base
    uint32_t fb_width;          // Width in pixels
    uint32_t fb_height;         // Height in pixels
    uint32_t fb_pitch;          // Bytes per scan line
    uint32_t fb_bpp;            // Bits per pixel

    // Memory map
    uint32_t mmap_count;        // Number of valid entries
    uint32_t _pad;              // Explicit padding
    MemoryMapEntry mmap[32];    // Memory map entries

} __attribute__((packed)) BootInfo;
```

BootInfo 结构体的设计遵循"信息足够但不过量"的原则。`entry_point` 告诉内核应该跳转到哪个地址（对于 flat binary 就是物理地址 0x20000，如果是 ELF 可能是高半内核地址）。`kernel_phys_base` 和 `kernel_size` 帮助内核了解自己被加载到哪里、占用多少内存。Framebuffer 信息来自 VESA 调用，内核可以用这些信息直接绘图。`mmap[]` 数组直接复用了 E820 的数据格式，内核可以遍历这个数组来做物理内存管理。

你可能注意到这个头文件会被两个不同的编译单元包含：bootloader 的 C 代码（-m32 编译）和内核的 C++ 代码（-m64 编译）。这就是为什么所有字段都使用显式大小的类型（uint32_t, uint64_t），而不是 size_t 或 long——这些类型在 32 位和 64 位下的宽度不同，会导致结构体布局不一致。`_pad` 字段也是为了对齐，确保 mmap 数组从 8 字节边界开始。

### 磁盘加载：从 8 扇区到 832 扇区

上一章我们只读取了 8 个扇区（4KB）的 ELF header，本章需要读取完整的小内核。`boot/common/boot.S` 中的 `load_kernel_from_disk` 函数做了重大更新：

```asm
// Mini kernel loading configuration
.set MINI_KERNEL_LBA,         16          // Starting LBA
.set MINI_KERNEL_SECTORS,     832         // Total sectors (416KB)
.set MINI_KERNEL_LOAD_PHYS,   0x20000     // Load address
.set MINI_KERNEL_LOAD_SEG,    0x2000      // Segment: 0x2000 << 4 = 0x20000
.set MINI_KERNEL_LOAD_OFF,    0x0000      // Offset
```

这里有个关键变化：加载地址从 0x10000 改成了 0x20000。原因有三：第一，0x10000 那个区域离 bootloader 代码区（0x8000）太近，一旦 bootloader 需要扩展空间就容易冲突；第二，我们打算在 0x90000 设置 Protected Mode 的栈，0x10000 到 0x90000 之间只有 512KB，而小内核本身就有 416KB，空间太紧；第三，0x20000 是一个"干净"的地址，16KB 对齐，方便后续做页表映射。

扇区数从 8 增加到 832，这意味着小内核的大小约为 416KB。这个数字是怎么来的？其实是我根据实际编译后的 `mini_kernel.bin` 的大小反推的。编译完成后用 `ls -l build/kernel/mini/mini_kernel.bin` 可以看到确切字节数，除以 512 向上取整就是所需扇区数。这个数字硬编码在 bootloader 里有点不优雅，但目前阶段能工作就行。后续可以改进为"先读第一扇区，解析头部获取大小，再读剩余部分"。

```asm
.load_loop:
    // Check if done
    cmpw $MINI_KERNEL_SECTORS, %bx
    jae .read_done

    // Calculate sectors for this iteration (max 127 per BIOS call)
    movw $MINI_KERNEL_SECTORS, %ax
    subw %bx, %ax                    // AX = remaining
    cmpw $DISK_MAX_SECTORS_PER_CALL, %ax
    jbe .read_count_ok
    movw $DISK_MAX_SECTORS_PER_CALL, %ax

.read_count_ok:
    movw %ax, %bp                    // BP = sectors to read this iteration
```

这里引入了分批读取的逻辑。INT 13h AH=0x42 每次最多读取 127 个扇区（这是 BIOS 规范的限制），而我们需要读取 832 个扇区，所以必须循环多次。BX 寄存器跟踪"已经读取的扇区数"，每次循环计算剩余扇区数，取 min(剩余, 127) 作为本次读取量。BP 保存本次要读的扇区数（因为后续 BIOS 调用会破坏 AX）。

```asm
    // Build DAP at fixed location 0x7B00
    movw $DAP_OFFSET, %di

    movb $16, %es:(%di)              // DAP.size = 16
    movb $0, %es:1(%di)              // DAP.reserved = 0
    movw %bp, %es:2(%di)             // DAP.count = BP

    // Fill DAP.buffer = MINI_KERNEL_LOAD_PHYS + (BX * 512)
    movw %bx, %ax
    shlw $5, %ax                     // AX *= 32
    addw $MINI_KERNEL_LOAD_SEG, %ax  // AX = 0x2000 + BX*32
    movw %ax, %es:6(%di)             // buffer segment
    movw $MINI_KERNEL_LOAD_OFF, %ax
    movw %ax, %es:4(%di)             // buffer offset = 0
```

DAP 的 buffer 字段需要动态计算。每次循环时，我们已经读取了 BX 个扇区，每个扇区 512 字节，所以当前缓冲区地址 = 基地址 + BX*512。在 Real Mode 的 segment:offset 表示法中，物理地址 = segment<<4 + offset。我们固定 offset=0，只调整 segment。计算公式：segment = (0x20000 + BX*512) >> 4 = 0x2000 + BX*32。这里用左移 5 位（乘以 32）代替除以 512 再右移 4 位，巧妙地避免了乘除法。

```asm
    // Fill DAP.lba = MINI_KERNEL_LBA + BX
    movw $MINI_KERNEL_LBA, %ax
    addw %bx, %ax
    movw %ax, %es:8(%di)             // LBA low 16 bits
    xorw %ax, %ax
    movw %ax, %es:10(%di)            // LBA high 16 bits
    movw %ax, %es:12(%di)
    movw %ax, %es:14(%di)            // Clear high DWORD (important!)
```

LBA 地址同样需要动态计算。当前 LBA = 起始 LBA (16) + 已读扇区数 (BX)。注意这里显式清零了 LBA 的高 32 位（偏移 12-15）。虽然对于小内核来说 LBA 肯定不超过 16 位，但 DAP 结构要求完整的 64 位 LBA，如果高位残留垃圾值，某些严格的 BIOS 可能会拒绝读取。我在调试时就是因为忘了清零高 32 位，导致 QEMU 的某些版本读取失败。

```asm
    // Critical: Save BX and BP across BIOS call
    pushw %bx
    pushw %bp

    // INT 13h AH=0x42 requires DS:SI (NOT ES:SI!)
    movw $DAP_SEGMENT, %dx
    movw %dx, %ds
    movw $DAP_OFFSET, %si
    movb $0x80, %dl
    movb $DISK_READ_CMD, %ah
    int $0x13

    jc .disk_error_restore_bp
    cmpb $0, %ah
    jne .disk_error_restore_bp

    // Restore on success path
    popw %bp
    popw %bx

    // Update counter
    addw %bp, %bx
    jmp .read_loop
```

这里有三个关键点。第一，BIOS 调用前保存 BX 和 BP，因为 BIOS 可能破坏这些寄存器（虽然规范说不会，但实际某些 BIOS 会）。第二，INT 13h AH=0x42 要求 DS:SI 指向 DAP，而不是 ES:SI——我第一次实现时用错了，结果怎么读都不成功。第三，错误处理路径要特别注意栈顺序：push BX, push BP 后，栈顶是 BP，然后是 BX，所以恢复时要先 pop BP 再 pop BX（错误路径用两个 label 分别处理成功和失败两种情况，确保栈平衡）。

### Protected Mode：本章无操作

`boot/stage2.S` 的 Protected Mode 入口在本章没有任何实质操作：

```asm
.code32
pm_entry:
    // Set up data segment registers
    movw $0x10, %ax
    movw %ax, %ds, %es, %fs, %gs, %ss

    // Set up new stack
    movl $0x90000, %esp

    // Debug output: 'P' for Protected Mode
    movb $0x50, %al
    outb %al, $0xE9

    // ============================================================
    // 004_boot_load_mini_kernel_B: No operation needed
    // ============================================================
    // Mini kernel was already loaded to 0x20000 in real mode.
    // Nothing to do in protected mode, proceed to long mode.

    // Setup page tables and enter long mode
    call setup_page_tables
    call enter_long_mode
```

你可能会问：既然内核已经加载完成了，为什么不直接在 Protected Mode 跳转？答案是我们需要进入 Long Mode 才能执行 64 位代码。小内核是用 `-mcmodel=large -m64` 编译的 64 位代码，在 32 位 Protected Mode 下无法运行。所以即使加载完成了，也必须继续完成页表设置、Long Mode 初始化，然后在 64 位模式下跳转。

### Long Mode 跳转：真正的交接

`boot/stage2.S` 的 Long Mode 入口是本章的核心：

```asm
.code64
long_mode_entry:
    // Set up data segment registers
    movw $GDT_DATA64, %ax
    movw %ax, %ds, %es, %fs, %gs, %ss

    // Set up 64-bit stack
    movabsq $0x90000, %rsp

    // Verify long mode entry
    movb $CHAR_LONG_MODE, %al       // 'L'
    outb %al, $DEBUGCON_PORT

    // ============================================================
    // Jump to mini kernel
    // ============================================================
    // Output 'J' to debugcon (Jumping to kernel)
    movb $0x4A, %al                 // 'J'
    outb %al, $0xE9

    // Calculate kernel entry point
    // Mini kernel is loaded at physical 0x20000
    // For flat binary, entry point = load address
    movabsq $0x20000, %rax

    // Jump to kernel
    jmp *%rax

    // Should never reach here
    cli
.lm_halt:
    hlt
    jmp .lm_halt
```

这里的跳转逻辑非常直接。小内核以 flat binary 格式加载到物理地址 0x20000，其入口点 `_start` 就在这个地址。我们只需要把这个地址加载到 RAX，然后用 `jmp *%rax` 间接跳转。为什么用间接跳转而不是直接 `jmp $0x20000`？因为 64 位模式下直接绝对跳转的编码方式有限制，用寄存器间接跳转是最通用可靠的方式。

'J' 字符的输出是关键的验证点。如果看到 debugcon 输出 'J'，说明 bootloader 成功到达跳转点。如果随后看到 'M'（mini kernel 输出），说明跳转成功，内核接管了 CPU。如果只有 'J' 没有 'M'，说明跳转失败了——可能是内核二进制损坏、入口点错误，或者内核代码有 bug 导致立即崩溃。

### 小内核入口：接管控制权

`kernel/mini/main.cpp` 是小内核的主函数：

```cpp
extern "C" {
#include <stdint.h>
}

static void debugcon_putc(char c) {
    __asm__ volatile("outb %0, $0xE9" : : "a"(c));
}

extern "C" [[noreturn]] void mini_kernel_main(uint64_t boot_info_addr) {
    (void)boot_info_addr;  // Unused for now

    // Output 'M' to debugcon to verify we reached here
    debugcon_putc('M');

    // Halt
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

这个函数极其简单，但有几个重要细节。首先，`boot_info_addr` 参数是 bootloader 通过 RDI 寄存器传递的（System V AMD64 ABI 规定第一个整数参数在 RDI）。虽然本章我们没有填充 BootInfo 结构体（留到下一章），但参数已经在那里了。其次，`debugcon_putc` 用内联汇编实现，避免了依赖任何标准库。最后，无限循环 `cli; hlt` 让 CPU 停机——因为没有中断，HLT 会一直停着，不会消耗 CPU 资源。

真正跳转到 `mini_kernel_main` 之前，还有一层汇编入口 `kernel/mini/arch/x86_64/boot.S`：

```asm
.section .text
.code64

.global _start
_start:
    cli

    /* Setup stack */
    movq $__mini_stack_top, %rsp
    xorq %rbp, %rbp

    /* Clear BSS */
    movq $__bss_start, %rdi
    movq $__bss_end, %rcx
    subq %rdi, %rcx
    xorq %rax, %rax
    rep stosb

    /* Save BootInfo pointer */
    movq %rdi, __boot_info_ptr

    /* Call C++ main */
    movq __boot_info_ptr, %rdi
    call mini_kernel_main

.halt:
    cli
    hlt
    jmp .halt
```

这个汇编 stub 做了几件重要的事情。首先设置栈——linker.ld 在 BSS 后面预留了 8KB 的栈空间。然后清零 BSS 段，这是 C/C++ 标准的要求（未初始化的全局变量必须为零）。保存 BootInfo 指针到 `__boot_info_ptr`，这样内核的其他代码也能访问。最后调用 `mini_kernel_main`，如果它返回了（虽然它标记为 `[[noreturn]]`），就进入死循环 halt。

### 链接器脚本：确保正确的地址

`kernel/mini/linker.ld` 定义了内核的内存布局：

```ld
OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)

SECTIONS
{
    /* Kernel loads at physical 0x20000 */
    . = 0x20000;

    .text : { *(.text) }
    .rodata : { *(.rodata) }
    .data : { *(.data) }

    .bss : {
        __bss_start = .;
        *(.bss)
        *(COMMON)
        __bss_end = .;
    }

    /DISCARD/ : {
        *(.comment*)
        *(.note*)
        *(.eh_frame*)
    }
}
```

这个链接脚本非常简洁。`. = 0x20000` 设置代码段的起始地址为 0x20000，这与 bootloader 的加载地址完全匹配。`.text`, `.rodata`, `.data` 段依次排列，`.bss` 段最后，并在其中定义了 `__bss_start` 和 `__bss_end` 符号供汇编代码使用。`/DISCARD/` 段丢弃不需要的 section，减少最终二进制大小。

这里有个微妙但重要的细节：linker.ld 指定的是物理地址（因为我们直接跳转到物理地址），而不是高半内核地址（如 0xFFFFFFFF80000000）。这是因为本章我们使用 flat binary 格式（objcopy -O binary），这种格式不包含任何地址重定位信息，加载到哪就在哪执行。如果用高半内核地址，需要开启页表映射并设置正确的虚拟地址，那属于下一章的内容。

---

## 设计决策深度分析

### 决策 1：Flat Binary vs ELF 加载

**问题**：小内核应该以什么格式加载？Flat binary（原始二进制）还是 ELF 格式？

**本项目的做法**：我们采用 flat binary 格式。编译过程是：C++ 源码 -> ELF64 (linker.ld) -> objcopy -O binary -> mini_kernel.bin。Bootloader 直接把 mini_kernel.bin 当作连续的字节流加载到 0x20000，然后跳转到这个地址执行。ELF 的加载地址和实际地址一致，不需要任何重定位。

**备选方案**：另一种设计是 bootloader 解析 ELF header，按照 program header 表加载各个 segment 到指定地址，然后跳转到 entry point。GRUB 就是这么做的，它支持完整的 ELF64 加载，能处理复杂的内存布局。或者用 multiboot 协议，让 multiboot-compliant bootloader（如 GRUB）加载我们的内核。

**为什么不选备选方案**：ELF 解析会显著增加 bootloader 复杂度。需要读取 ELF header（e_phoff, e_phnum），遍历 program header table（p_type, p_paddr, p_filesz, p_memsz），按 p_offset 从文件读取，按 p_paddr 写入内存，还要处理 BSS 段清零。对于我们的 mini kernel 来说，这些复杂度都不必要——只有一个 TEXT+DATA 段，没有复杂的需求。Flat binary 是最简单直接的：把文件当黑盒，加载到固定地址，跳转执行。

**如果要扩展/改进**：当前方案要求内核链接地址与加载地址完全一致。如果想支持高半内核（如 0xFFFFFFFF80000000），需要开启页表映射，或者在 bootloader 里实现 ELF 解析和重定位。另一个改进方向是支持"位置无关代码"（PIC），用 -fPIC 编译内核，这样内核可以加载到任意地址而不需要重定位。但 PIC 会带来性能开销（需要通过 GOT 访问全局变量），对于 OS 内核通常不是最优选择。

### 决策 2：Real Mode 完成全部磁盘读取

**问题**：磁盘读取应该在哪个阶段完成？Real Mode、Protected Mode 还是 Long Mode？

**本项目的做法**：我们在 Real Mode 的最后阶段（进入 Protected Mode 之前）完成全部磁盘读取。`load_kernel_from_disk` 在 stage2.S 里被调用，位置在 VESA 操作之后、`cli`（准备切换模式）之前。一旦读取完成，内核镜像就安全地躺在 0x20000，后续的模式切换不会再动它。

**备选方案**：理论上可以在 Protected Mode 或 Long Mode 读取磁盘。Protected Mode 下可以用 BIOS 的 32-bit protected mode BIOS calls（如果有），或者直接访问磁盘控制器的 I/O 端口（如 ATA/ATAPI PIO 模式）。Long Mode 下需要写磁盘驱动程序（如 AHCI 或 NVMe），那复杂度就更高了。

**为什么不选备选方案**：Protected Mode 下调用 BIOS 很麻烦——需要设置调用门（call gate）或者在切换回 Real Mode 调用 BIOS 再切回来。直接访问磁盘控制器需要理解硬件协议（ATA command set、端口映射等），而且 PIO 模式很慢。Long Mode 下必须写驱动，那是内核的事情，bootloader 不应该涉及那么深。Real Mode 是 BIOS 调用的"黄金窗口"，放弃这个机会纯属自找麻烦。

**如果要扩展/改进**：当前实现是"一次性读取全部"，如果内核很大（比如几十 MB），读取时间会很长，用户会觉得启动卡住了。可以改进为"异步读取"：先读取核心部分，跳转进去，内核在后台继续读取剩余部分。这需要 bootloader 和内核的配合，比如用共享内存或环形缓冲区传递数据。另一个改进是添加进度显示，每读取 64KB 输出一个点，让用户知道进度。

### 决策 3：固定地址 0x20000 vs 可配置加载地址

**问题**：小内核应该加载到哪个地址？硬编码 0x20000 还是通过某种协议协商？

**本项目的做法**：我们硬编码了 0x20000 作为加载地址。Bootloader 中的 `MINI_KERNEL_LOAD_PHYS = 0x20000`，linker.ld 中的 `. = 0x20000`，stage2.S 中的跳转目标 `movabs $0x20000, %rax`，三者必须一致。任何一处改了但其他地方没改，系统就会崩溃。

**备选方案**：更灵活的设计是通过配置文件或链接脚本约定加载地址。比如在某个固定位置（如 0x7E00）放一个描述符，说明"内核加载到 0xXXXXX，入口点在 0xYYYYY"。或者实现一个"可重定位内核"，内核代码能在任意地址运行，bootloader 根据当前内存情况选择合适的加载位置。

**为什么不选备选方案**：对于当前阶段，固定地址是最简单可靠的。配置文件需要解析逻辑，增加代码复杂度。可重定位内核需要编译为 PIC 或者处理全局变量重定位，这对内核来说不合适（性能开销和实现复杂度）。硬编码虽然不灵活，但在 bootloader-内核的"二进制契约"里是常见的——Linux 内核早期也是固定地址加载。

**如果要扩展/改进**：可以引入"内核头"（kernel header）的概念。在 mini_kernel.bin 的开头放一个固定格式的 header，包含 magic number、内核大小、入口点偏移、所需内存等信息。Bootloader 先读取第一扇区，解析 header，然后决定加载策略。这样既能保持灵活性，又不需要复杂的外部配置。另一个方向是支持"多内核选择"，比如 LBA 16-xxx 是内核 A，LBA yyy-zzz 是内核 B，bootloader 根据某个"启动标志"选择加载哪个，实现简单的 A/B 更新机制。

---

## 常见变体与扩展方向

下面列出几个你可以尝试的扩展实验：

1. **⭐ 添加启动进度显示**：在 `load_kernel_from_disk` 循环中，每读取 64KB 输出一个点（`.`）到 debugcon，这样可以看到"加载中..."的进度，避免长时间无输出的焦虑感。

2. **⭐ 实现 CRC32 校验**：在编译 mini kernel 时计算 CRC32 checksum，写入 kernel header 的某个字段。Bootloader 读取后验证 checksum，如果校验失败显示"Checksum error"并 halt，防止加载损坏的内核。

3. **⭐⭐ 填充 BootInfo 结构体**：当前 BootInfo 只是定义了，没有实际填充。你可以修改 stage2.S，在跳转前填充 BootInfo 的各个字段（entry_point=0x20000, fb_info 从 VESA 调用结果复制, mmap 从 E820 结果复制），然后把 BootInfo 地址通过 RDI 传递给内核。

4. **⭐⭐ 支持内核命令行参数**：在某个固定地址（如 0x7E00）放一个字符串，作为"内核命令行"。Mini kernel 启动时读取这个字符串，解析参数（如 `debug=1`, `video=1024x768`），实现简单的启动配置。

5. **⭐⭐⭐ 实现 ELF 解析器**：放弃 flat binary，让 bootloader 能解析 ELF64 格式的内核。读取 ELF header，获取 e_phoff 和 e_phnum，遍历 program header table，按照 p_paddr 和 p_filesz 加载每个 segment，清零 BSS 段，最后跳转到 e_entry。这是真正的 bootloader 该有的样子。

---

## 参考资料

### Intel/AMD 手册

* **Intel SDM Vol. 3A, Section 9.1.2**: Control Registers — CR0.PE 和 CR0.PG 位的作用
* **Intel SDM Vol. 2A, Chapter 3**: Instruction Set Reference — JMP 指令的各种编码方式
* **Intel SDM Vol. 3A, Section 15.3.2**: INT 15h, Other Legacy Services — E820 之外的其他 BIOS 服务
* **AMD64 Architecture Programmer's Manual Vol. 2**, Section 5.4: Data Types — 32 位和 64 位下的数据类型差异

### OSDev Wiki

* [Entering Long Mode Directly](https://wiki.osdev.org/Entering_Long_Mode_Directly) — 直接跳转到 Long Mode 的完整流程
* [Segment:Offset Addressing](https://wiki.osdev.org/Segment:Offset_Addressing) — Real Mode 的地址计算详解
* [Bootloader](https://wiki.osdev.org/Bootloader) — Bootloader 开发的综合指南
* [Executable File Formats](https://wiki.osdev.org/Executable_File_Formats) — ELF、PE 等可执行文件格式对比

### 其他资源

* [x86 Boot Documentation](https://www.kernel.org/doc/html/latest/x86/boot.html) — Linux 内核的 x86 启动协议规范
* [The Boot Process](https://wiki.osdev.org/The_Boot_Process) — 从 MBR 到内核的完整启动流程
* [ELF for the ARM Architecture](https://static.docs.arm.com/ihi0044/g/aaelf32.pdf) — ELF 格式的官方规范（虽然针对 ARM，但 ELF64 的核心结构是通用的）
* [OSDev Serial Port](https://wiki.osdev.org/Serial_Port) — 串口和 debugcon 的使用方法

---

到这里就大功告成了。本章我们完成了从"能读取"到"能执行"的关键跨越。虽然 mini kernel 只是输出一个字符然后 halt，但这个简单的字符'M'代表着我们成功地让 bootloader 把控制权移交给了自定义代码——这是任何 OS 开发者的第一次"真正意义上的成功"。

下一章我们会在内核里做更多有趣的事情：解析 BootInfo，输出内存布局信息，在 VESA framebuffer 上绘制文字，实现简单的内存管理器。到时候，我们就能看到一个更像"操作系统"的东西了。但在此之前，好好品味一下这个启动过程：从 MBR 的 512 字节开始，到 Stage2，到 Real Mode 的 BIOS 调用，到 Protected Mode 和 Long Mode 的切换，最终跳转到我们自己的代码。每一步都是精心设计的，每一步都有其历史和技术原因。理解了这些，你就理解了 x86 启动的精髓。
