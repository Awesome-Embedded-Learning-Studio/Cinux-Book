# 008 Mini Kernel 磁盘驱动与 ELF 加载器 —— 从磁盘加载大内核

## 章节导语

上一章我们把中断系统搞定了，内核终于能在异常发生时体面地报告错误，而不是一言不合就 triple fault。但说实话，到目前为止我们这个 mini kernel 还是个"一次性"的东西——Bootloader 把它从磁盘加载到内存里，它跑完就永远停在 `cli; hlt` 的死循环里，什么都干不了。

问题出在哪？Mini kernel 受限于 Bootloader 的加载能力，只能被放在 0x20000 开始的那块区域里，最大 416KB。这个空间塞不下一个真正有用的内核——我们需要一个"加载器中的加载器"：mini kernel 本身充当一个迷你的 bootloader，从磁盘读取一个更大的 ELF 格式的内核，把它搬到正确的内存位置，然后跳过去执行。这就是这一章要做的事情。

完成本章后，我们会看到 mini kernel 成功初始化 ATA 磁盘控制器，从磁盘读取 MBR 扇区并验证引导签名 0xAA55，然后读取 mini kernel 自身的头部来演示 ELF 解析能力。虽然真正的"大内核"还不存在（那是后续 milestone 的事），但整个加载管线已经就位——ATA 驱动、ELF 解析器、内核加载器三剑客全部到位，只差一个真正的大内核 ELF 文件放上去就能跑。

本章的前置知识是上一章（007_mini_kernel_intr）的中断系统，因为 ATA PIO 驱动涉及大量的 I/O 端口轮询操作，如果中途中断处理出了问题，整个读取流程会直接卡死。

---

## 概念精讲

### ATA PIO 是什么？为什么不直接用 BIOS 中断？

如果你跟我一样是从实模式开始折腾的，你可能会问：读取磁盘不是可以用 `INT 13h` 吗？确实，Bootloader 的 Stage2 就是用 BIOS 中断读磁盘的。但问题在于，我们现在已经处于 long mode 下了——64 位保护模式里根本没有 BIOS 中断可用，所有的实模式服务都已经在切换模式的那一刻灰飞烟灭。

所以我们只能直接和硬件对话。ATA（Advanced Technology Attachment）是 x86 平台上最经典的硬盘接口协议，即使现在的主流 SATA/NVMe 硬盘在软件层面也保持了 ATA 兼容性。QEMU 模拟的硬盘就是一个标准的 ATA 设备，我们可以通过 I/O 端口直接发送命令来读取扇区。

PIO（Programmed I/O）是 ATA 传输方式中最简单的一种——CPU 主动轮询数据端口，一个字一个字地把数据从磁盘控制器读进来。没有 DMA，没有中断通知，就是纯粹的"发了命令就等着，数据准备好了就读"。效率当然不高，但对于一个 bootloader 级别的磁盘读取来说，简单可靠才是第一位的。

```
ATA PIO 读取流程：

CPU → 发送 READ SECTORS 命令到 I/O 端口 0x1F7
     ↓
CPU → 轮询状态端口 0x1F7，等待 DRQ 位变 1（数据就绪）
     ↓
CPU → 从数据端口 0x1F0 连续读取 256 个 16 位字 = 512 字节 = 1 个扇区
     ↓
重复以上步骤直到所有扇区读完
```

### ATA 控制器的 I/O 端口布局

x86 平台上，主 ATA 控制器使用 `0x1F0` 到 `0x1F7` 这组 I/O 端口，外加一个控制端口 `0x3F6`。每个端口的功能是固定的：

- `0x1F0`：数据端口（Data Register），16 位宽度，读取扇区数据就是从这里面一个字一个字地取
- `0x1F2`：扇区计数寄存器，告诉控制器要读几个扇区
- `0x1F3/4/5`：LBA 地址的低/中/高字节，定位要读的扇区位置
- `0x1F6`：驱动器选择寄存器，选择主/从盘以及 LBA 模式
- `0x1F7`：状态寄存器（读）/ 命令寄存器（写），这是最重要的端口——发送命令靠写它，检查状态靠读它

其中 LBA（Logical Block Addressing）是现代磁盘的寻址方式，你可以把它理解为"扇区编号"。LBA 0 就是第一个扇区（MBR），LBA 1 是第二个扇区（Stage2 的开头），以此类推。相比古老的 CHS（柱面-磁头-扇区）寻址，LBA 就是一个简单的线性编号，对软件来说友好得多。

### ELF 格式为什么是加载大内核的关键？

我们在之前的章节里，内核是被 Bootloader 以"裸二进制"（flat binary）的方式加载的——`objcopy -O binary` 把 ELF 文件里的所有内容按地址平铺成一个连续的二进制块，没有段信息，没有入口点信息，Bootloader 只能盲目地把它放到固定地址然后跳转。这种方式简单粗暴，但缺点也很明显：内核必须链接到一个固定的物理地址，而且所有段（代码、数据、BSS）必须连续排列。

ELF（Executable and Linkable Format）格式就灵活得多。ELF 文件里有程序头表（Program Header Table），每个条目描述一个"段"（segment）的加载信息——它在文件中的偏移（p_offset）、要加载到的虚拟地址（p_vaddr）和物理地址（p_paddr）、在文件中占多少字节（p_filesz）、在内存中占多少字节（p_memsz）。特别是 p_memsz 可以大于 p_filesz，多出来的部分就是 BSS——需要清零但不需要从文件读取。这些信息让加载器能够精确地把每个段放到正确的位置。

对我们的 higher-half 内核来说，ELF 格式尤其重要。内核代码链接在 `0xFFFFFFFF80000000` 以上的虚拟地址空间，但 mini kernel 运行在物理地址身份映射模式下，需要把内核加载到对应的物理地址。ELF 头里的入口点（e_entry）是虚拟地址，加载器需要减去 `0xFFFFFFFF80000000` 才能得到物理入口——这就是 `load_elf` 函数最后那段 higher-half 地址转换在做的事。

```
ELF 加载过程：

磁盘上的 ELF 文件:
┌──────────────┐
│  ELF Header  │  e_entry = 0xFFFFFFFF80001000 (virtual)
│  (64 bytes)  │  e_phoff → Program Header Table
├──────────────┤
│  Phdr[0]     │  PT_LOAD: p_paddr=0x100000, p_filesz=0x2000, p_memsz=0x3000
│  Phdr[1]     │  PT_LOAD: p_paddr=0x200000, p_filesz=0x1000, p_memsz=0x1000
├──────────────┤
│  Segment 0   │  代码段数据 (0x2000 bytes)
│  Segment 1   │  数据段数据 (0x1000 bytes)
└──────────────┘

加载到内存后:
物理地址 0x100000:  [代码段 0x2000 字节][BSS 清零 0x1000 字节]
物理地址 0x200000:  [数据段 0x1000 字节]

入口点物理地址 = 0xFFFFFFFF80001000 - 0xFFFFFFFF80000000 = 0x1000
```

### LBA28 与 LBA48 的区别

ATA 规范有两种 LBA 寻址方式。LBA28 使用 28 位地址，最多能寻址 2^28 个扇区 = 128GB。对于小磁盘来说够用，但现代硬盘轻松超过这个容量。LBA48 使用 48 位地址，寻址空间达到 2^48 个扇区 = 128PB，这在未来很长一段时间内都够用了。

在代码里我们做了一个自动判断：如果 LBA 地址超过 28 位范围（`0x10000000`），或者要读的扇区数超过 256（LBA28 一次最多读 256 个扇区），就自动切换到 LBA48 模式。实际上在 QEMU 的 1MB 磁盘镜像里，LBA28 绰绰有余，但代码里两种模式都实现了，为了将来对接更大的虚拟磁盘做准备。

### Freestanding 环境下的 memset 和 memcpy

这一章我们还新增了 `lib/string.h` 和 `lib/string.cpp`——手动实现的 `memset`、`memcpy`、`memmove`。你可能会觉得奇怪，这些不是标准库里最基础的函数吗？但别忘了我们的内核是 freestanding 环境，编译时带了 `-ffreestanding -nostdlib`，没有任何标准库可用。而 ELF 加载器在拷贝段数据和清零 BSS 的时候必须用 `memcpy` 和 `memset`，所以只能自己写。

这三个函数的实现非常直白——逐字节操作。其中 `memmove` 比 `memcpy` 多了一点讲究：它需要处理源地址和目标地址重叠的情况。如果目标在源前面，从前到后复制；如果目标在源后面，从后到前复制——否则覆盖了还没读的数据。虽然在我们当前的加载场景里不太会遇到重叠，但作为一个通用工具函数，做对了总是好的。

---

## 动手实现

### 第一步——实现 freestanding 内存工具函数

**目标**：提供 `memset`、`memcpy`、`memmove` 三个函数，供 ELF 加载器在拷贝段数据和清零 BSS 时使用。

**代码**（文件路径：`kernel/mini/lib/string.h`）：

```cpp
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void* memset(void* dest, int val, size_t count);
void* memcpy(void* __restrict__ dest, const void* __restrict__ src, size_t count);
void* memmove(void* dest, const void* src, size_t count);

#ifdef __cplusplus
}
#endif
```

**代码**（文件路径：`kernel/mini/lib/string.cpp`）：

```cpp
#include "string.h"

void* memset(void* dest, int val, size_t count) {
    uint8_t* d = static_cast<uint8_t*>(dest);
    uint8_t v = static_cast<uint8_t>(val);
    for (size_t i = 0; i < count; i++) {
        d[i] = v;
    }
    return dest;
}

void* memcpy(void* __restrict__ dest, const void* __restrict__ src, size_t count) {
    uint8_t* d = static_cast<uint8_t*>(dest);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < count; i++) {
        d[i] = s[i];
    }
    return dest;
}

void* memmove(void* dest, const void* src, size_t count) {
    uint8_t* d = static_cast<uint8_t*>(dest);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    if (d < s) {
        for (size_t i = 0; i < count; i++) d[i] = s[i];
    } else if (d > s) {
        for (size_t i = count; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dest;
}
```

这几个函数用 `extern "C"` 包裹，因为 ELF 加载器的 C++ 代码会调用它们，而我们希望使用 C 语言的链接约定避免 name mangling。`__restrict__` 关键字告诉编译器 `dest` 和 `src` 不会指向同一块内存，编译器可以据此做更激进的优化——不过在我们这个逐字节拷贝的实现里，这个提示更多是语义上的声明。

**验证**：这一步没有独立的运行验证，它会在后续步骤中被间接使用。

---

### 第二步——搭建 ATA PIO 磁盘驱动

**目标**：实现对 ATA 主控制器（I/O 端口 0x1F0-0x1F7）的初始化和扇区读取功能，使用 PIO 轮询模式。

**代码**（文件路径：`kernel/mini/driver/ata.hpp`，关键常量与接口）：

```cpp
namespace cinux::mini::driver::ata {

constexpr uint16_t ATA_PRIMARY_BASE = 0x1F0;  // 主控制器基址
constexpr uint16_t ATA_PRIMARY_CTRL = 0x3F6;  // 控制端口

// 寄存器偏移量
constexpr uint16_t ATA_REG_DATA       = 0;  // 数据端口 (16-bit)
constexpr uint16_t ATA_REG_SECTOR_CNT = 2;  // 扇区计数
constexpr uint16_t ATA_REG_LBA_LOW    = 3;  // LBA [0:7]
constexpr uint16_t ATA_REG_LBA_MID    = 4;  // LBA [8:15]
constexpr uint16_t ATA_REG_LBA_HIGH   = 5;  // LBA [16:23]
constexpr uint16_t ATA_REG_DRIVE      = 6;  // 驱动器选择
constexpr uint16_t ATA_REG_STATUS     = 7;  // 状态（读）/ 命令（写）

// 状态位
constexpr uint8_t ATA_STATUS_DRQ  = 0x08;  // 数据就绪
constexpr uint8_t ATA_STATUS_RDY  = 0x40;  // 驱动器就绪
constexpr uint8_t ATA_STATUS_BSY  = 0x80;  // 驱动器忙

// 命令
constexpr uint8_t ATA_CMD_READ_PIO     = 0x20;  // LBA28 读
constexpr uint8_t ATA_CMD_READ_PIO_EXT = 0x24;  // LBA48 读

constexpr uint16_t ATA_SECTOR_SIZE = 512;

bool init();  // 初始化控制器
bool read(uint64_t lba, uint16_t count, void* buffer);  // 读取扇区
}
```

头文件把所有 ATA 相关的常量都定义为 `constexpr`，包括 I/O 端口编号、状态位掩码、命令码。这些值都是 ATA 规范里固定的，不能改也不能猜——如果你手滑把 `0x1F0` 写成 `0x1F7`，读出来的就不是数据而是状态寄存器的值了，排查这种问题能让你怀疑人生。

**代码**（文件路径：`kernel/mini/driver/ata.cpp`，初始化部分）：

```cpp
#include "ata.hpp"
#include "driver/io.h"
#include "lib/kprintf.h"

namespace cinux::mini::driver::ata {

static bool s_initialized = false;

namespace {

inline uint8_t read_reg(uint16_t reg) {
    return io::inb(ATA_PRIMARY_BASE + reg);
}

inline void write_reg(uint16_t reg, uint8_t value) {
    io::outb(ATA_PRIMARY_BASE + reg, value);
}

inline uint16_t read_data() {
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(ATA_PRIMARY_BASE));
    return value;
}

void delay_400ns() {
    // 读 4 次控制端口，每次约 100ns
    io::inb(ATA_PRIMARY_CTRL);
    io::inb(ATA_PRIMARY_CTRL);
    io::inb(ATA_PRIMARY_CTRL);
    io::inb(ATA_PRIMARY_CTRL);
}

bool wait_not_busy() {
    for (uint32_t i = 0; i < 10000000; i++) {
        uint8_t status = read_reg(ATA_REG_STATUS);
        if ((status & ATA_STATUS_BSY) == 0) return true;
        __asm__ volatile("pause");
    }
    return false;
}

}  // anonymous namespace
```

这里有几个值得细说的地方。`read_data()` 使用 `inw` 指令（16 位 I/O 读取），因为 ATA 数据端口是 16 位宽的——每次读一个 word，256 次 word 读取刚好是一个 512 字节的扇区。`delay_400ns()` 是 ATA 规范要求的：发完命令之后必须等至少 400ns 才能读状态寄存器，否则可能读到过期的状态值。在真实硬件上读一次 I/O 端口大约需要 100ns，所以连续读 4 次就能满足时序要求。注意我们读的是控制端口（0x3F6）而不是状态端口（0x1F7），因为读状态端口有副作用（会清除中断标志位）。

**代码**（文件路径：`kernel/mini/driver/ata.cpp`，初始化函数）：

```cpp
bool init() {
    kprintf("[INIT] Initializing ATA controller...\n");

    // 软件复位：置位 SRST（bit 1），禁用中断（bit 2 = nIEN）
    io::outb(ATA_PRIMARY_CTRL, 0x04);
    delay_400ns();
    io::outb(ATA_PRIMARY_CTRL, 0x00);  // 取消复位
    delay_400ns();

    if (!wait_not_busy()) {
        kprintf("[ATA] ERROR: drive did not come out of reset\n");
        return false;
    }

    // 选择主盘，LBA 模式
    write_reg(ATA_REG_DRIVE, 0xE0);  // ATA_DRIVE_MASTER
    delay_400ns();

    if (!wait_not_busy()) {
        kprintf("[ATA] ERROR: master drive not ready\n");
        return false;
    }

    uint8_t status = read_reg(ATA_REG_STATUS);
    if (status == 0xFF) {
        kprintf("[ATA] ERROR: no drive detected (floating bus)\n");
        return false;
    }

    s_initialized = true;
    kprintf("[INIT] ATA controller initialized successfully (status=0x%02x).\n", status);
    return true;
}
```

初始化过程分三步：先做软件复位（往控制端口写 0x04 再写 0x00），等待驱动器就绪（BSY 位清零），然后选择主盘（`0xE0` = LBA 模式 + 主盘）。状态值为 `0xFF` 是一个特殊的"浮空总线"标志——说明根本没有设备连接，这在物理机上拔掉硬盘时会遇到，但在 QEMU 里不应该发生。如果你在 QEMU 里看到这个错误，那大概率是 I/O 端口地址写错了。

**代码**（文件路径：`kernel/mini/driver/ata.cpp`，读取函数核心逻辑）：

```cpp
bool read(uint64_t lba, uint16_t count, void* buffer) {
    if (!s_initialized || count == 0 || buffer == nullptr) return false;

    if (!wait_not_busy()) return false;

    // 自动选择 LBA28 或 LBA48
    bool use_lba48 = (lba >= 0x10000000ULL) || (count > 256);

    if (use_lba48) {
        // LBA48: 先发高 16 位 LBA 和高字节扇区数
        write_reg(ATA_REG_DRIVE, 0xE0 | 0x40);  // LBA48 标志位
        delay_400ns();
        write_reg(ATA_REG_SECTOR_CNT, (count >> 8) & 0xFF);
        write_reg(ATA_REG_LBA_LOW, (lba >> 24) & 0xFF);
        write_reg(ATA_REG_LBA_MID, (lba >> 32) & 0xFF);
        write_reg(ATA_REG_LBA_HIGH, (lba >> 40) & 0xFF);
        // 再发低 16 位
        write_reg(ATA_REG_SECTOR_CNT, count & 0xFF);
        write_reg(ATA_REG_LBA_LOW, lba & 0xFF);
        write_reg(ATA_REG_LBA_MID, (lba >> 8) & 0xFF);
        write_reg(ATA_REG_LBA_HIGH, (lba >> 16) & 0xFF);
        write_reg(ATA_REG_COMMAND, 0x24);  // READ SECTORS EXT
    } else {
        // LBA28: 地址和扇区数各写一次
        write_reg(ATA_REG_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
        delay_400ns();
        write_reg(ATA_REG_SECTOR_CNT, count & 0xFF);
        write_reg(ATA_REG_LBA_LOW, lba & 0xFF);
        write_reg(ATA_REG_LBA_MID, (lba >> 8) & 0xFF);
        write_reg(ATA_REG_LBA_HIGH, (lba >> 16) & 0xFF);
        write_reg(ATA_REG_COMMAND, 0x20);  // READ SECTORS
    }

    // 逐扇区读取数据
    auto* buf = static_cast<uint16_t*>(buffer);
    for (uint16_t sector = 0; sector < count; sector++) {
        delay_400ns();
        // 等待 DRQ 位
        for (uint32_t i = 0; i < 10000000; i++) {
            uint8_t status = read_reg(ATA_REG_STATUS);
            if (status & 0x01) { /* ERROR */ return false; }
            if (status & 0x20) { /* DRIVE FAULT */ return false; }
            if (!(status & ATA_STATUS_BSY) && (status & ATA_STATUS_DRQ)) break;
            __asm__ volatile("pause");
        }
        // 一个扇区 = 256 个 16 位 word
        for (int word = 0; word < 256; word++) {
            buf[word] = read_data();
        }
        buf += 256;  // 前进 512 字节
    }
    return true;
}
```

LBA48 和 LBA28 的命令序列有个容易搞混的地方：LBA48 模式下，地址和扇区数的高位字节要先发、低位字节后发，相当于要往同一组寄存器写两次。这是因为 ATA 规范设计了一个"扩展"机制——先发的高位字节会被控制器缓存，等到低位字节发完再拼成一个完整的 48 位地址。如果你把高低位的发送顺序搞反了，读取到的数据就是错的，而且不会报错——这种静默错误最恶心。

另外注意我们把 buffer 转成了 `uint16_t*`，因为 `inw` 每次读 16 位。一个扇区 512 字节 = 256 个 word，外层循环每读完一个扇区就前进 256 个 word 的位置。整个读取过程是阻塞的——没有 DMA，没有中断回调，就是老老实实地等数据就绪然后搬进来。

**验证**：构建运行后应该看到 `[INIT] ATA controller initialized successfully` 的输出。

---

### 第三步——搭建 ELF64 解析器和加载器

**目标**：实现 ELF64 文件头验证、PT_LOAD 段加载、BSS 清零，以及 higher-half 入口地址转换。

**代码**（文件路径：`kernel/mini/elf_loader.hpp`，关键类型定义）：

```cpp
namespace cinux::mini::elf_loader {

// ELF 标识常量
constexpr uint8_t  ELF_CLASS_64 = 2;      // 64-bit ELF
constexpr uint8_t  ELF_DATA_LSB = 1;      // Little-endian
constexpr uint16_t ET_EXEC      = 2;      // 可执行文件
constexpr uint16_t EM_X86_64    = 62;     // x86-64 架构
constexpr uint32_t PT_LOAD      = 1;      // 可加载段

// ELF64 文件头 (64 字节)
struct Elf64_Ehdr {
    uint8_t  e_ident[16];    // 魔数 + 分类信息
    uint16_t e_type;         // 文件类型
    uint16_t e_machine;      // 目标架构
    uint32_t e_version;
    uint64_t e_entry;        // 入口点（虚拟地址）
    uint64_t e_phoff;        // 程序头表偏移
    uint64_t e_shoff;        // 节头表偏移
    uint32_t e_flags;
    uint16_t e_ehsize;       // ELF 头大小 (64)
    uint16_t e_phentsize;    // 程序头条目大小 (56)
    uint16_t e_phnum;        // 程序头数量
    // ... 省略 section header 相关字段
} __attribute__((packed));

// ELF64 程序头 (56 字节)
struct Elf64_Phdr {
    uint32_t p_type;     // 段类型 (PT_LOAD = 1)
    uint32_t p_flags;    // 段权限 (R/W/X)
    uint64_t p_offset;   // 段数据在文件中的偏移
    uint64_t p_vaddr;    // 虚拟地址
    uint64_t p_paddr;    // 物理地址
    uint64_t p_filesz;   // 文件中的大小
    uint64_t p_memsz;    // 内存中的大小 (≥ filesz, 多出部分是 BSS)
    uint64_t p_align;    // 对齐
} __attribute__((packed));

bool parse_elf_header(const void* elf);
size_t calculate_kernel_size(const Elf64_Ehdr* ehdr);
uint64_t load_elf(void* elf_src, uint64_t staging_size);
}
```

这两个结构体是 ELF64 规范中明确定义的，字段名和含义与标准文档完全一致。`__attribute__((packed))` 确保编译器不会插入填充字节——ELF 文件头是精确的二进制格式，差一个字节整个解析就全错了。我们只关心 PT_LOAD 类型的段，因为只有这些段才需要被加载到内存中。其他类型的段（比如 PT_DYNAMIC、PT_NOTE）对于静态链接的内核来说没有意义。

**代码**（文件路径：`kernel/mini/elf_loader.cpp`，头验证函数）：

```cpp
bool parse_elf_header(const void* elf) {
    if (elf == nullptr) return false;
    const auto* ehdr = static_cast<const Elf64_Ehdr*>(elf);

    // 验证魔数 0x7F 'E' 'L' 'F'
    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        kprintf("[ELF] ERROR: invalid magic\n");
        return false;
    }

    // 必须是 64 位、小端序、x86-64、可执行
    if (ehdr->e_ident[4] != ELF_CLASS_64)  return false;  // 不是 64 位
    if (ehdr->e_ident[5] != ELF_DATA_LSB) return false;   // 不是小端
    if (ehdr->e_machine != EM_X86_64)      return false;   // 不是 x86-64
    if (ehdr->e_type != ET_EXEC)           return false;   // 不是可执行文件
    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) return false;  // 没有程序头

    return true;
}
```

验证函数检查了五个条件：魔数（确保这确实是一个 ELF 文件）、64 位类别（我们只支持 ELF64）、小端序（x86 就是小端的）、x86-64 架构（不能把 ARM 内核塞进来）、可执行类型（我们不能加载 .o 目标文件或 .so 共享库）。每一步失败都会在串口打印错误信息——这些信息在调试"为什么加载失败"的时候非常关键。

**代码**（文件路径：`kernel/mini/elf_loader.cpp`，加载函数）：

```cpp
uint64_t load_elf(void* elf_src, uint64_t staging_size) {
    if (!parse_elf_header(elf_src)) return 0;
    auto* ehdr = static_cast<Elf64_Ehdr*>(elf_src);

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr* phdr = /* get_phdr helper */;

        if (phdr->p_type != PT_LOAD) continue;  // 跳过非加载段

        // 安全检查：段数据不能超出 staging buffer
        if (phdr->p_offset + phdr->p_filesz > staging_size) {
            kprintf("[ELF] ERROR: segment exceeds staging buffer\n");
            return 0;
        }

        uint64_t dest_addr = phdr->p_paddr;
        const void* src = (const uint8_t*)elf_src + phdr->p_offset;

        // 1. 拷贝文件数据
        if (phdr->p_filesz > 0) {
            memcpy((void*)dest_addr, src, phdr->p_filesz);
        }

        // 2. 清零 BSS（memsz > filesz 的部分）
        if (phdr->p_memsz > phdr->p_filesz) {
            uint64_t bss_start = dest_addr + phdr->p_filesz;
            size_t bss_size = phdr->p_memsz - phdr->p_filesz;
            memset((void*)bss_start, 0, bss_size);
        }
    }

    // 转换 higher-half 入口地址为物理地址
    constexpr uint64_t HIGHER_HALF_BASE = 0xFFFFFFFF80000000ULL;
    uint64_t entry = ehdr->e_entry;
    if (entry >= HIGHER_HALF_BASE) {
        entry -= HIGHER_HALF_BASE;
    }
    return entry;
}
```

加载函数的核心逻辑只有三步：遍历程序头，对每个 PT_LOAD 段做拷贝 + BSS 清零，最后返回入口点。但这里面有一个 `staging_size` 参数特别重要——它是我们从磁盘读取的字节总数，用来做边界检查。ELF 文件里的 `p_offset + p_filesz` 表示这个段的数据在文件中的范围，如果这个范围超出了我们实际读取的字节数，说明 ELF 文件比我们读入的数据要大，这时候继续读取就是越界访问了——读到的是内存里的垃圾数据，不是真正的段内容。宁可在这里报错终止，也不要带着垃圾数据继续跑，否则大内核的行为是完全不可预测的。

Higher-half 地址转换的逻辑很直白：如果入口点大于等于 `0xFFFFFFFF80000000`，就减去这个基址得到物理地址。这个 `0xFFFFFFFF80000000` 是我们在链接脚本里定义的内核虚拟基地址（`KERNEL_VMA`），mini kernel 运行时还没有开启分页，不能直接访问这个虚拟地址，所以必须转换成物理地址才能跳转。

**验证**：这一步需要组合到 main.cpp 中才能验证，见第五步。

---

### 第四步——搭建大内核加载管线

**目标**：把 ATA 驱动和 ELF 加载器串起来，实现一个完整的"从磁盘读取 → 解析 ELF → 加载段 → 返回入口"的流水线。

**代码**（文件路径：`kernel/mini/big_kernel_loader.hpp`，关键常量）：

```cpp
namespace cinux::mini::loader {

constexpr uint64_t MINI_KERNEL_LOAD_ADDR = 0x20000;    // mini kernel 物理加载地址
constexpr uint64_t BIG_KERNEL_LOAD_ADDR  = 0x1000000;  // 16MB: ELF staging buffer
constexpr uint64_t BIG_KERNEL_LBA        = 848;         // 大内核在磁盘上的起始扇区
constexpr uint16_t BIG_KERNEL_MAX_SECTORS = 512;        // 最多读 512 扇区 = 256KB

uint64_t load_big_kernel(uint64_t disk_lba);
}
```

这里定义了几个关键的布局参数。`BIG_KERNEL_LOAD_ADDR = 0x1000000`（16MB）是 ELF 文件的暂存区（staging buffer）地址。为什么选 16MB？因为 mini kernel 被加载在 0x20000（128KB），PMM 管理的物理内存从 1MB 开始，16MB 是一个安全的中间位置——既不会和 mini kernel 冲突，也不会和 Bootloader 的数据结构冲突。磁盘上大内核从 LBA 848 开始——扇区 0 是 MBR，1-15 是 Stage2，16-847 是 mini kernel（最多 832 个扇区 = 416KB），848 开始就是大内核的地盘了。

**代码**（文件路径：`kernel/mini/big_kernel_loader.cpp`）：

```cpp
#include "big_kernel_loader.hpp"
#include "driver/ata.hpp"
#include "elf_loader.hpp"
#include "lib/kprintf.h"

namespace cinux::mini::loader {

uint64_t load_big_kernel(uint64_t disk_lba) {
    kprintf("[LOADER] Loading big kernel from disk LBA 0x%x...\n", disk_lba);

    constexpr uint32_t staging_bytes =
        static_cast<uint32_t>(BIG_KERNEL_MAX_SECTORS) * driver::ata::ATA_SECTOR_SIZE;

    // Step 1: 从磁盘读入 staging buffer
    if (!driver::ata::read(disk_lba, BIG_KERNEL_MAX_SECTORS,
                           reinterpret_cast<void*>(BIG_KERNEL_LOAD_ADDR))) {
        kprintf("[LOADER] ERROR: Failed to read from disk!\n");
        return 0;
    }

    // Step 2: 验证 ELF 魔数
    const auto* magic = reinterpret_cast<const uint8_t*>(BIG_KERNEL_LOAD_ADDR);
    if (magic[0] != 0x7F || magic[1] != 'E' || magic[2] != 'L' || magic[3] != 'F') {
        kprintf("[LOADER] ERROR: No ELF magic at staging buffer!\n");
        return 0;
    }

    // Step 3: 调用 ELF 加载器解析和加载
    uint64_t entry = elf_loader::load_elf(
        reinterpret_cast<void*>(BIG_KERNEL_LOAD_ADDR), staging_bytes);

    if (entry == 0) {
        kprintf("[LOADER] ERROR: ELF loading failed!\n");
        return 0;
    }

    kprintf("[LOADER] Big kernel loaded. Entry point: 0x%p\n", entry);
    return entry;
}

}  // namespace cinux::mini::loader
```

`load_big_kernel` 是整个加载管线的入口，它按顺序做了三件事：调用 ATA 驱动从磁盘读取数据到 staging buffer，验证前四个字节是不是 ELF 魔数，然后调用 ELF 加载器解析段并搬运到目标地址。返回值是物理入口点地址——调用者拿到这个地址后就可以直接 `jmp` 过去了。当然目前大内核还不存在，这个函数在 milestone 008 里不会被 main.cpp 调用，但管线已经就位，后续章节只要把编译好的大内核 ELF 镜像追加到磁盘镜像里就行。

**验证**：这一步同样是组合验证，见第五步。

---

### 第五步——整合到 main.cpp：读取 MBR 并验证

**目标**：在 mini_kernel_main 中初始化 ATA 驱动，读取 MBR 扇区验证引导签名，读取 mini kernel 头部演示 ELF 解析。

**代码**（文件路径：`kernel/mini/main.cpp`，新增部分）：

```cpp
#include "driver/ata.hpp"
#include "elf_loader.hpp"

static uint8_t g_sector_buf[512] __attribute__((aligned(16)));

extern "C" [[noreturn]] void mini_kernel_main(uint64_t boot_info_addr) {
    // ... 之前的 BootInfo / GDT / IDT / PMM 初始化 ...

    // 初始化 ATA 磁盘驱动
    if (!cinux::mini::driver::ata::init()) {
        kprintf("[INIT] ERROR: ATA initialization failed!\n");
        while (1) __asm__ volatile("cli; hlt");
    }

    // 读取 MBR (LBA 0) 并验证引导签名
    kprintf("[DEMO] Reading MBR (LBA 0)...\n");
    if (cinux::mini::driver::ata::read(0, 1, g_sector_buf)) {
        uint16_t sig = (uint16_t)g_sector_buf[510] |
                       ((uint16_t)g_sector_buf[511] << 8);
        kprintf("[DEMO] MBR boot signature: 0x%04x %s\n", sig,
                sig == 0xAA55 ? "(VALID)" : "(INVALID)");
    }

    // 读取 mini kernel (LBA 16) 检查 ELF 头
    // 注意：mini kernel 实际上是 flat binary（objcopy 转换后的），
    // 所以这里不会有 ELF 魔数——但这是对 parse_elf_header 的一个反面测试
    kprintf("[DEMO] Reading mini kernel header (LBA 16)...\n");
    if (cinux::mini::driver::ata::read(16, 1, g_sector_buf)) {
        if (cinux::mini::elf_loader::parse_elf_header(g_sector_buf)) {
            kprintf("[DEMO] ELF header detected at LBA 16\n");
        } else {
            kprintf("[DEMO] No valid ELF header at LBA 16 (expected for flat binary)\n");
        }
    }

    kprintf("\n[MINI] Milestone 008 complete. Waiting for big kernel (009+)...\n");
    while (1) __asm__ volatile("cli; hlt");
}
```

这里我们做了两件有意义的事。第一件是读取 MBR 扇区并验证引导签名——MBR 的第 510-511 字节必须是 `0x55 0xAA`（小端序读出来就是 `0xAA55`），这是 x86 BIOS 规范规定的引导标志。如果我们能正确读到这个值，说明 ATA 驱动的整个读取链路是通的：初始化 → 发命令 → 等数据 → 读数据 → 返回。

第二件是读取 LBA 16（mini kernel 所在的扇区）并尝试解析 ELF 头。这里你会看到输出 "No valid ELF header"——这是因为 mini kernel 在写入磁盘之前被 `objcopy -O binary` 转换成了 flat binary，ELF 头信息已经丢了。这其实是一个很好的反面测试，验证了 `parse_elf_header` 在面对非 ELF 数据时能正确拒绝，而不是崩溃或产生未定义行为。

同时别忘了更新 CMakeLists.txt，把新增的源文件注册进去：

```cmake
# kernel/mini/CMakeLists.txt 中的 mini_kernel_common
add_library(mini_kernel_common OBJECT
    # ... 之前的文件 ...
    driver/ata.cpp
    elf_loader.cpp
    big_kernel_loader.cpp
    lib/string.cpp
)
```

测试文件也需要注册：

```cmake
# kernel/mini/test/CMakeLists.txt
add_executable(mini_kernel_test
    # ... 之前的文件 ...
    test_ata.cpp            # ATA PIO tests (008)
    test_elf_loader.cpp     # ELF64 parser/loader tests (008)
)
```

**验证**：构建运行后应该看到以下关键输出。

---

## 构建与运行

```bash
# 从项目根目录
git checkout 008_mini_kernel_disk_and_loader
cmake -B build -DCMAKE_BUILD_TYPE=Debug -S .
cmake --build build -j$(nproc)
cd build && make run
```

**期望输出**：

```
Cinux Mini Kernel v0.1.0
BootInfo: entry_point=0xFFFFFFFF80020000, kernel_phys_base=0x20000
...
[INIT] Setting up GDT...
[INIT] GDT loaded successfully.
[INIT] Setting up IDT...
[INIT] IDT loaded successfully.
[MINI] PMM: Total 131040 pages (511 MB), Free 130784 pages (510 MB)

[TEST] Triggering breakpoint exception (int $3)...
...
[TEST] Breakpoint test passed! Execution continued after #BP.

[INIT] Initializing ATA controller...
[INIT] ATA controller initialized successfully (status=0x50).
[DEMO] Reading MBR (LBA 0)...
[DEMO] MBR boot signature: 0xaa55 (VALID)
[DEMO] Reading mini kernel header (LBA 16)...
[DEMO] No valid ELF header at LBA 16 (expected for flat binary)

[MINI] Milestone 008 complete. Waiting for big kernel (009+)...
```

看到 `MBR boot signature: 0xaa55 (VALID)` 就说明 ATA PIO 读取链路完全通了。`status=0x50` 拆开来看就是 `01010000b`——RDY 位（bit 6）和 DSC 位（bit 4）为 1，表示驱动器就绪且寻道完成，这是 QEMU 模拟硬盘的标准状态。

QEMU 的串口参数说明：`-serial stdio` 让串口输出直接打印在终端，我们的 `kprintf` 底层写 COM1 端口（0x3F8），所以终端上看到的文字就是内核的串口输出。`-device isa-debug-exit,iobase=0xf4,iosize=0x04` 提供了一个用于测试退出的 I/O 端口，内核测试框架通过写 `0x00` 到端口 `0xF4` 来让 QEMU 退出，但正常的 production 内核不会用到它。

---

## 调试技巧

**ATA 初始化卡住，没有输出**

这种情况多半是 I/O 端口地址写错了或者 `wait_not_busy` 死循环了。用 `make run-debug` 启动 GDB，在 `ata::init` 处设断点，单步跟踪看卡在哪一步。如果卡在第一次 `wait_not_busy` 调用，检查控制端口地址是否正确（`0x3F6`），以及复位序列是否正确（先写 `0x04` 再写 `0x00`）。也可以在 QEMU monitor 里用 `info qtree` 确认磁盘设备是否存在。

**读取 MBR 签名不是 0xAA55**

先确认磁盘镜像文件确实包含了 MBR。用 `xxd build/cinux.img | head 2` 看前 32 字节——应该能看到 MBR 的代码。然后检查 `xxd build/cinux.img | grep -A1 01f0` 看偏移 0x1FE-0x1FF（第 510-511 字节）是不是 `55aa`。如果磁盘镜像本身是对的但读取结果不对，很可能是 LBA 寄存器的写入顺序搞反了——ATA 规范要求先写扇区数、再写 LBA 低中高字节、最后发命令，顺序错了控制器会去读错误的扇区。

**ELF 解析失败：invalid magic**

如果在读取真正的 ELF 文件时看到这个错误，说明 staging buffer 里的数据不对。最可能的原因是磁盘上的 LBA 偏移不匹配——`big_kernel_loader.hpp` 里定义的 `BIG_KERNEL_LBA = 848` 必须和 `scripts/build_image.sh` 里实际的磁盘布局一致。用 `dd if=build/cinux.img bs=512 skip=848 count=1 | xxd | head` 看看那个扇区的内容是不是 ELF 魔数（`7f 45 4c 46`）。

**用 GDB 验证扇区读取数据**

```bash
(gdb) break cinux::mini::driver::ata::read
(gdb) continue
# 断在 read 函数入口
(gdb) print lba     # 检查 LBA 参数
(gdb) print count   # 检查扇区数
(gdb) print buffer  # 检查 buffer 地址
# 执行完读取后
(gdb) finish
(gdb) x/2bx ((uint8_t*)buffer) + 510  # 检查 MBR 签名位置
```

---

## 本章小结

| 组件 | 关键函数/结构 | 说明 |
|------|-------------|------|
| 内存工具 | `memset()`, `memcpy()`, `memmove()` | Freestanding 环境，手动实现的字节操作函数 |
| ATA 驱动 | `ata::init()`, `ata::read()` | PIO 轮询模式磁盘驱动，支持 LBA28/LBA48 |
| ATA 寄存器 | 0x1F0-0x1F7, 0x3F6 | 主控制器数据/状态/命令/控制端口 |
| ELF 解析 | `Elf64_Ehdr`, `Elf64_Phdr`, `parse_elf_header()` | ELF64 头验证、段信息读取 |
| ELF 加载 | `load_elf()` | PT_LOAD 段拷贝、BSS 清零、higher-half 地址转换 |
| 内核加载器 | `load_big_kernel()` | 串联 ATA + ELF 的完整加载管线 |
| 关键常量 | `BIG_KERNEL_LOAD_ADDR=0x1000000`, `BIG_KERNEL_LBA=848` | staging buffer 地址和磁盘布局参数 |

下一章（009）我们会真正构建一个大内核，把它追加到磁盘镜像里，然后让 mini kernel 通过这一章搭建的加载管线读取它、解析它、跳转到它——届时整个 Bootloader → mini kernel → big kernel 的三级启动链就完整了。我们在这一章里写好的 ATA 驱动和 ELF 加载器就是连接 mini kernel 和 big kernel 的桥梁。
