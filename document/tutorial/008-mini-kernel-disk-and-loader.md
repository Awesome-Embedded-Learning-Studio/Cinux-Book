# 从磁盘加载大内核：手写 ATA 驱动 + ELF 加载器，让 mini kernel 真正成为"引导程序"

> 作者：
> 标签：x86-64, ATA PIO, IDE 磁盘驱动, ELF64 加载器, LBA28/LBA48, 程序头解析, 裸机开发, C++, QEMU, OS 开发

---

## 前言

到上章为止，我们的 mini kernel 已经能启动、能打印、能管理物理内存、能捕获中断异常了——说实话已经是个挺像样的小内核了。但有一个问题一直在回避：mini kernel 的体积是被严格限制的，最多 416KB。这个限制不是我们故意设定的，而是 x86 实模式 + Bootloader 阶段的内存布局决定的：内核被加载在 0x20000，往上到 0x88000 就到顶了，再往上就撞到保护模式栈（0x90000）。

所以真正的"大内核"——那个未来会包含文件系统、进程管理、网络协议栈的完整 OS——不可能塞进 416KB 里。我们需要一个全新的加载策略：mini kernel 本身不做复杂的事，它只充当一个"二次引导程序"（second-stage loader），负责从磁盘上把大内核的 ELF 二进制读出来、解析好、放到正确的内存位置，然后跳转过去执行。

这一章我们要做的就是这个完整链路。具体来说，我们要写三个模块：一个 ATA PIO 磁盘驱动，让内核能直接通过 I/O 端口和 IDE 控制器通信读取磁盘扇区；一个 ELF64 解析器，把从磁盘读出来的 ELF 二进制文件中的 PT_LOAD 段搬运到它们应去的物理地址上；还有一个大内核加载器（big kernel loader），把前两者串起来形成完整的加载管线。完成之后的效果是这样的：QEMU 串口会输出 ATA 初始化信息、磁盘读取结果、ELF 头验证结果，以及最终的段加载日志。虽然大内核本身还不存在（那是后续 milestone 的事），但加载管线已经完全就绪，随时可以把一个真正的 ELF 内核二进制放上去跑起来。

## 环境说明

实验环境还是老一套：x86_64 平台，GNU AS + GCC/G++ + CMake 构建，QEMU 模拟运行。内核是 freestanding C++23，无标准库、无异常、无 RTTI。

磁盘布局的几个关键参数需要提前交代清楚。目前 Cinux 的磁盘镜像（`cinux.img`）是 1MB 大小，由 `scripts/build_image.sh` 构建，布局如下：扇区 0 是 MBR（512 字节），扇区 1-15 是 Stage2 引导程序（最多 7680 字节），扇区 16 开始是 mini kernel（最多 416KB = 832 扇区）。所以大内核的起始位置被定在 LBA 848（16 + 832），这个常量在 `big_kernel_loader.hpp` 里定义为 `BIG_KERNEL_LBA = 848`。

内存方面，mini kernel 运行在 0x20000，我们选择把大内核的 ELF 原始数据先放到一个暂存区（staging buffer）里，地址选在 0x1000000（16MB），这个位置既不会和 mini kernel 冲突，也不会和 Bootloader 的结构体冲突。等 ELF 解析完成之后，PT_LOAD 段会被搬运到它们各自的目标物理地址。

## 第一步——搞清楚 ATA PIO 是怎么回事

在写代码之前，我们需要理解 x86 平台上"读磁盘"到底意味着什么。现代操作系统通常使用 DMA（Direct Memory Access）或者 NVMe 协议来和磁盘通信，但在内核开发的早期阶段，这些机制太复杂了——我们需要的是最简单、最可靠的方式，先让磁盘能读再说。

ATA PIO（Programmed I/O）就是那个最简单的方式。它的原理很直接：CPU 通过 `in`/`out` 指令直接读写 IDE 控制器的 I/O 端口，一个字节一个字节地把数据搬进来。主 IDE 控制器使用 I/O 端口范围 0x1F0-0x1F7（数据寄存器到状态寄存器）和 0x3F6（控制寄存器），每个端口都有固定的功能。数据端口 0x1F0 是 16 位宽的，每次 `inw` 读一个 16 位字，读一个完整的 512 字节扇区需要 256 次 `inw` 操作。

你可以把它理解为：ATA PIO 相当于 CPU 亲自去磁盘的仓库门口排队搬货，每次搬一个箱子（16 位字），搬完 256 个箱子凑齐一个扇区。DMA 则相当于雇了一个专门的搬运工（DMA 控制器），CPU 只需要告诉它"把 N 号仓库到 M 号仓库的东西搬到内存的 X 地址"，然后就可以去干别的事了。在我们的场景下，CPU 亲自搬货完全够用，而且调试起来非常直观——每一步都是同步的，读完了再继续，不会出现异步回调的混乱。

PIO 的另一个好处是它不依赖中断——我们用的是轮询（polling）模式：发完读命令之后，CPU 不停地检查状态寄存器的 BSY（忙）和 DRQ（数据就绪）位，等到磁盘说"数据准备好了"，再去数据端口读。这种方式的缺点是 CPU 时间全浪费在等待上了，但在 mini kernel 这个场景下，加载内核是一次性的操作，性能不是瓶颈。

## 第二步——实现 ATA PIO 驱动

现在我们对 ATA PIO 的机制有了概念上的理解，接下来直接上手写代码。驱动被拆分为头文件 `ata.hpp` 和实现文件 `ata.cpp`，放在 `kernel/mini/driver/` 目录下。

首先是端口和常量定义，这些都在头文件中声明为 `constexpr` 常量：

```cpp
namespace cinux::mini::driver::ata {

constexpr uint16_t ATA_PRIMARY_BASE = 0x1F0;
constexpr uint16_t ATA_PRIMARY_CTRL = 0x3F6;

constexpr uint16_t ATA_REG_DATA       = 0;
constexpr uint16_t ATA_REG_SECTOR_CNT = 2;
constexpr uint16_t ATA_REG_LBA_LOW    = 3;
constexpr uint16_t ATA_REG_LBA_MID    = 4;
constexpr uint16_t ATA_REG_LBA_HIGH   = 5;
constexpr uint16_t ATA_REG_DRIVE      = 6;
constexpr uint16_t ATA_REG_STATUS     = 7;

constexpr uint8_t ATA_STATUS_BSY  = 0x80;
constexpr uint8_t ATA_STATUS_RDY  = 0x40;
constexpr uint8_t ATA_STATUS_DRQ  = 0x08;
constexpr uint8_t ATA_STATUS_ERR  = 0x01;

constexpr uint8_t ATA_CMD_READ_PIO     = 0x20;
constexpr uint8_t ATA_CMD_READ_PIO_EXT = 0x24;

constexpr uint8_t ATA_DRIVE_MASTER = 0xE0;
constexpr uint16_t ATA_SECTOR_SIZE  = 512;
```

你会发现这些端口号和位掩码不是我们随意编的——它们是 ATA/IDE 规范里写死的。0x1F0 是主 IDE 通道的标准 I/O 基地址，从 0x1F0 到 0x1F7 分别对应数据寄存器、错误/特性寄存器、扇区计数、LBA 低位/中位/高位、驱动器选择和状态/命令寄存器。状态寄存器的位含义也是固定的：最高位 BSY 表示磁盘正在忙，次高位 RDY 表示磁盘就绪，第 3 位 DRQ 表示数据缓冲区已准备好可以读写了。

接下来是初始化函数 `init()`。ATA 控制器在可用之前需要执行一次软件复位（software reset），复位的方法是往控制寄存器（0x3F6）写 0x04（断言 SRST 位），等 400 纳秒，然后再写 0x00（释放 SRST）。这里有一个容易忽略的细节：400 纳秒怎么等？在裸机环境下我们没有 `sleep` 函数，也没有高精度定时器。ATA 规范给出的方案是读 4 次备用状态寄存器——每次 I/O 端口读取在典型硬件上大约需要 100 纳秒，4 次就是 400 纳秒。

```cpp
void delay_400ns() {
    io::inb(ATA_PRIMARY_CTRL);
    io::inb(ATA_PRIMARY_CTRL);
    io::inb(ATA_PRIMARY_CTRL);
    io::inb(ATA_PRIMARY_CTRL);
}
```

复位之后，我们要等磁盘报 RDY（就绪）并确认总线不是浮空的（0xFF 表示没有磁盘连接）。这些检查看起来简单，但如果没有做的话，在真实硬件上会立刻出问题——QEMU 里一切顺利不代表真机也如此，很多模拟器对 ATA 控制器的模拟是大幅简化过的。

初始化通过之后，接下来是核心的 `read()` 函数——从指定 LBA 读取指定数量的扇区到缓冲区。这个函数支持两种寻址模式：LBA28 和 LBA48。LBA28 最多寻址 2^28 个扇区（约 128GB），LBA48 扩展到了 2^48（约 128PB）。当 LBA 地址超过 28 位或者单次读取超过 256 个扇区时，我们自动切换到 LBA48 模式。

两种模式的命令序列有所不同。LBA28 模式下，我们把 24 位 LBA 拆成三个字节分别写入 LBA_LOW、LBA_MID、LBA_HIGH 寄存器，高 4 位放进 DRIVE 寄存器的低 4 位，然后发送 0x20 命令。LBA48 模式则需要把地址和计数各发两遍——先发高 16 位再发低 16 位，然后发送 0x24 命令。这个"先高后低"的顺序不是随便定的，而是 ATA 规范对 LBA48 命令的硬性要求。

命令发出后，就进入逐扇区读取的循环。对每个扇区，我们先等 DRQ 置位（表示 512 字节数据已就绪），然后连续执行 256 次 `inw`（每次读 16 位），把数据写入缓冲区：

```cpp
for (uint16_t sector = 0; sector < count; sector++) {
    delay_400ns();
    if (!wait_data_ready()) {
        kprintf("[ATA] ERROR: failed reading sector %u (LBA %u)\n", sector,
                static_cast<uint32_t>(lba + sector));
        return false;
    }
    auto* buf = static_cast<uint16_t*>(buffer);
    for (int word = 0; word < 256; word++) {
        buf[word] = read_data();
    }
    buf += 256;
}
```

这里有一个值得一提的地方：我们把 buffer 转成 `uint16_t*` 来做 `inw` 操作。这是因为 ATA 数据端口是 16 位宽的，每次必须读 16 位——如果你用 `inb` 逐字节读，虽然也能工作但效率减半，而且某些 ATA 控制器实现可能不保证逐字节读取的正确性。另外缓冲区需要对齐到 16 位边界，否则在某些架构上可能出现未对齐访问错误。在我们的代码里用 `__attribute__((aligned(16)))` 保证了对齐。

## 第三步——理解 ELF64 的加载机制

磁盘驱动写好了，能从磁盘读原始字节了。但读出来的东西不是一个可以直接跳转执行的 flat binary——它是一个 ELF 格式的可执行文件，里面有文件头、程序头表、各种段，我们需要解析它才能正确加载。

ELF（Executable and Linkable Format）是 Unix/Linux 世界里最通用的可执行文件格式。一个 ELF64 文件的开头是一个 64 字节的 ELF 头（`Elf64_Ehdr`），包含魔数（0x7F 'E' 'L' 'F'）、架构类型（x86-64）、入口地址、程序头表偏移等信息。紧跟着的是若干程序头（`Elf64_Phdr`），每个 56 字节，描述一个"段"——可以是代码段、数据段、只读数据段等等。

对我们来说最重要的段类型是 `PT_LOAD`（类型值 1），它表示"这个段需要被加载到内存中"。每个 PT_LOAD 段有几个关键字段：`p_offset` 是段数据在 ELF 文件中的偏移，`p_filesz` 是段在文件中的大小，`p_memsz` 是段在内存中需要占用的总大小，`p_paddr` 是段的目标物理地址，`p_vaddr` 是段的目标虚拟地址。

`p_filesz` 和 `p_memsz` 的区别是理解 ELF 加载的关键。对于代码段和已初始化数据段，这两个值相等——文件里有多少字节，内存里就放多少字节。但对于 BSS 段（未初始化的全局变量和静态变量存放的地方），`p_memsz` 会大于 `p_filesz`，多出来的部分需要在加载时用零填充。这就是为什么 ELF 加载器除了做 memcpy 之外，还需要做 memset 清零。

整个加载过程可以理解为一个"拆包再组装"的过程：ELF 文件是一个打包好的运输箱，里面东西的摆放位置（`p_offset`）是按照文件存储优化的，和实际运行时需要的内存布局（`p_paddr`）不一样。加载器的工作就是按照程序头的指示，把每个段从箱子里的原始位置取出来，放到内存中正确的位置上，BSS 部分用零填好，最后告诉调用方"入口地址在这里"。

## 第四步——实现 ELF64 解析器

理解了机制之后，代码就很清晰了。ELF 解析器在 `kernel/mini/elf_loader.hpp` 和 `kernel/mini/elf_loader.cpp` 中实现。

首先是 ELF 头结构的定义。这些结构直接对应 ELF64 规范的二进制布局，所以必须用 `__attribute__((packed))` 防止编译器插入填充字节：

```cpp
struct Elf64_Ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

struct Elf64_Phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed));
```

接下来是头验证函数 `parse_elf_header()`。这个函数做了一连串检查：魔数是否为 0x7F 'E' 'L' 'F'，是否为 64 位 ELF，是否为小端编码，是否为 x86-64 架构，是否为可执行文件（ET_EXEC），以及程序头表是否存在。每一步检查失败都会通过 `kprintf` 输出具体的错误信息——这在调试的时候非常重要，因为你需要一个明确的"为什么不对"而不是一个笼统的"出错了"。

核心加载函数是 `load_elf()`，它接受两个参数：一个指向内存中 ELF 数据的指针，以及一个暂存区大小（用于边界检查）。函数遍历所有程序头，跳过非 PT_LOAD 的段，对每个 PT_LOAD 段执行以下操作：

先做边界检查——确认段数据（`p_offset + p_filesz`）没有超出暂存区的实际大小。这一步特别重要，因为我们从磁盘读的扇区数是固定的（BIG_KERNEL_MAX_SECTORS = 512），如果大内核的 ELF 比暂存区还大，不检查就会读到未初始化的内存垃圾。

然后是关键的搬运和清零操作：

```cpp
uint64_t dest_addr = phdr->p_paddr;
const void* src = reinterpret_cast<const uint8_t*>(elf_src) + phdr->p_offset;

if (phdr->p_filesz > 0) {
    memcpy(reinterpret_cast<void*>(dest_addr), src, phdr->p_filesz);
}

if (phdr->p_memsz > phdr->p_filesz) {
    uint64_t bss_start = dest_addr + phdr->p_filesz;
    size_t bss_size = static_cast<size_t>(phdr->p_memsz - phdr->p_filesz);
    memset(reinterpret_cast<void*>(bss_start), 0, bss_size);
}
```

这里用了 `memcpy` 和 `memset`——别忘我们在 freestanding 环境下没有标准库，这两个函数是自己实现的，在 `kernel/mini/lib/string.cpp` 中。实现本身很朴素：`memcpy` 就是逐字节拷贝，`memset` 就是逐字节写入。`memmove` 多了一步判断：如果源和目标有重叠且目标地址更高，就从后往前拷贝，避免覆盖还没读到的数据。

加载完成后，函数从 ELF 头中读取入口地址 `e_entry`。这里有一个小细节需要处理：我们的大内核是 higher-half 设计的，入口地址是一个类似 `0xFFFFFFFF80000000` 的虚拟地址，但 mini kernel 当前运行在 identity mapping 下，没法直接用这个虚拟地址跳转。所以我们需要把它转换成物理地址——如果入口地址大于 `0xFFFFFFFF80000000`（higher-half 基址），就减去这个基址得到物理地址，否则直接使用：

```cpp
constexpr uint64_t HIGHER_HALF_BASE = 0xFFFFFFFF80000000ULL;
uint64_t entry = ehdr->e_entry;
if (entry >= HIGHER_HALF_BASE) {
    entry = entry - HIGHER_HALF_BASE;
}
return entry;
```

## 第五步——串起完整加载管线

现在我们有了磁盘驱动和 ELF 解析器，还需要一个模块把两者串起来，形成从"磁盘上的原始字节"到"内存中可跳转的内核"的完整管线。这就是 `big_kernel_loader` 的工作。

加载策略是这样的：大内核的 ELF 二进制存放在磁盘的 LBA 848 开始的位置，我们用 ATA 驱动一次读取 512 个扇区（256KB）到 0x1000000 的暂存区，然后调用 ELF 解析器处理暂存区中的数据。512 个扇区是一个保守的上限，如果实际的大内核更小，ELF 解析器只会处理它找到的段，多读的磁盘数据不会被使用。

```cpp
uint64_t load_big_kernel(uint64_t disk_lba) {
    constexpr uint32_t staging_bytes =
        static_cast<uint32_t>(BIG_KERNEL_MAX_SECTORS) * driver::ata::ATA_SECTOR_SIZE;

    if (!driver::ata::read(disk_lba, BIG_KERNEL_MAX_SECTORS,
                           reinterpret_cast<void*>(BIG_KERNEL_LOAD_ADDR))) {
        kprintf("[LOADER] ERROR: Failed to read big kernel from disk!\n");
        return 0;
    }

    const auto* magic = reinterpret_cast<const uint8_t*>(BIG_KERNEL_LOAD_ADDR);
    if (magic[0] != 0x7F || magic[1] != 'E' || magic[2] != 'L' || magic[3] != 'F') {
        kprintf("[LOADER] ERROR: No ELF magic at staging buffer!\n");
        return 0;
    }

    uint64_t entry = elf_loader::load_elf(
        reinterpret_cast<void*>(BIG_KERNEL_LOAD_ADDR), staging_bytes);

    kprintf("[LOADER] Big kernel loaded successfully.\n");
    kprintf("[LOADER] Entry point: 0x%p\n", entry);
    return entry;
}
```

你会发现我们在调用 ELF 解析器之前先做了一个快速魔法数检查——读取暂存区的前四个字节看是不是 0x7F 'E' 'L' 'F'。这不是多余的，因为 `load_elf` 内部虽然也有头验证，但如果磁盘上的数据根本不是 ELF 文件（比如大内核还没放上去，读到的全零），快速检查可以避免进入更复杂的解析逻辑，也能给出更明确的错误信息。

## 第六步——在 main.cpp 中验证整个链路

最后，我们在 `mini_kernel_main` 中把所有东西集成起来。由于大内核目前还不存在（那是后续 milestone 的事），我们这一步的验证目标是确认 ATA 驱动能正确读取磁盘扇区，ELF 解析器能正确验证文件头。

```cpp
extern "C" [[noreturn]] void mini_kernel_main(uint64_t boot_info_addr) {
    BootInfo* boot_info = (BootInfo*)__boot_info_ptr;
    (void)boot_info_addr;

    kprintf("Cinux Mini Kernel v0.1.0\n");
    // ... Boot Info 打印、GDT/IDT/PMM 初始化 ...

    if (!cinux::mini::driver::ata::init()) {
        kprintf("[INIT] ERROR: ATA initialization failed!\n");
        while (1) __asm__ volatile("cli; hlt");
    }

    // 验证读取：读 MBR 看引导签名
    kprintf("[DEMO] Reading MBR (LBA 0)...\n");
    if (cinux::mini::driver::ata::read(0, 1, g_sector_buf)) {
        uint16_t sig = static_cast<uint16_t>(g_sector_buf[510]) |
                       (static_cast<uint16_t>(g_sector_buf[511]) << 8);
        kprintf("[DEMO] MBR boot signature: 0x%04x %s\n", sig,
                sig == 0xAA55 ? "(VALID)" : "(INVALID)");
    }

    // 验证解析：读 LBA 16 看有没有 ELF 头
    kprintf("[DEMO] Reading mini kernel header (LBA 16)...\n");
    if (cinux::mini::driver::ata::read(16, 1, g_sector_buf)) {
        if (cinux::mini::elf_loader::parse_elf_header(g_sector_buf)) {
            kprintf("[DEMO] ELF header detected at disk LBA 16 (mini kernel)\n");
        } else {
            kprintf("[DEMO] No valid ELF header at LBA 16 (expected for flat binary)\n");
        }
    }

    kprintf("\n[MINI] Milestone 008 complete. Waiting for big kernel (009+)...\n");
    while (1) __asm__ volatile("cli; hlt");
}
```

第一个测试读取磁盘的 LBA 0（MBR 所在的扇区），检查最后两个字节是否为 0xAA55——这是 x86 MBR 的标准引导签名，能验证 ATA 驱动确实从磁盘读到了正确的数据。第二个测试读取 LBA 16（mini kernel 的起始扇区），尝试用 ELF 解析器验证它。这里有一个有趣的点：mini kernel 本身是以 flat binary 格式存放在磁盘上的（由 `objcopy -O binary` 转换），不是 ELF 格式，所以 ELF 验证会失败——这是预期行为。但这个测试验证了从 LBA 16 读取数据是成功的，说明 ATA 驱动在整个磁盘范围内都能正确工作。

## 点火测试

构建并运行一下看看效果：

```bash
cd build && cmake --build . -j$(nproc) && make run
```

串口输出应该是这样的（截取关键部分）：

```
Cinux Mini Kernel v0.1.0
[INIT] Setting up GDT...
[INIT] GDT loaded successfully.
[INIT] Setting up IDT...
[INIT] IDT loaded successfully.
[INIT] Initializing ATA controller...
[INIT] ATA controller initialized successfully (status=0x50).
[DEMO] Reading MBR (LBA 0)...
[DEMO] MBR boot signature: 0xaa55 (VALID)
[DEMO] Reading mini kernel header (LBA 16)...
[ELF] ERROR: invalid magic: 00 00 00 00
[DEMO] No valid ELF header at LBA 16 (expected for flat binary)

[MINI] Milestone 008 complete. Waiting for big kernel (009+)...
```

你会注意到几个关键信息：ATA 控制器初始化成功（status=0x50，RDY=1，BSY=0），MBR 的引导签名 0xAA55 被正确读出来了——这证明磁盘驱动工作正常。LBA 16 处没有 ELF 魔数是因为 mini kernel 是 flat binary 不是 ELF 文件，前几个字节是代码而不是 0x7F 'E' 'L' 'F'，这完全符合预期。

## 一些值得记住的坑

写 ATA 驱动的时候最容易踩的第一个坑就是忘了做软件复位。如果你直接开始发读命令而不先复位，在某些环境（尤其是真实硬件）上控制器可能处于一个不确定的状态，读出来的数据全是垃圾。QEMU 对这个问题不太敏感因为它启动时控制器就是干净的，但真机上一定会出问题。

第二个坑是 400 纳秒等待。ATA 规范要求在特定操作之间插入 400ns 延迟，最典型的是发出命令之后、检查状态之前。如果你省掉这个延迟直接开始轮询状态，可能读到过时的状态值——上一次命令的状态还没更新，你以为磁盘准备好了但其实还在处理。我们通过读 4 次控制寄存器来达成这个延迟，这是 OS 开发里的标准做法，Linux 内核和很多引导程序也是这么干的。

第三个坑关于 ELF 加载器中的暂存区大小检查。这个检查很容易被忽略——很多人会想"反正读的扇区数够多，不会超的"。但如果有一天大内核变得特别大，超过了 512 扇区的上限，暂存区里只有部分数据，而 ELF 程序头指向的段偏移超出了实际读入的数据范围，不加检查的话 memcpy 就会读到暂存区之外的未初始化内存，加载出来的内核就是损坏的。这种 bug 非常隐蔽，因为它不会立刻崩溃——内核可能看起来正常启动了，但某些数据段的值是随机的垃圾值，等到运行到依赖这些数据的代码时才会出现不可预测的行为。

## 收尾

到这里，我们完成了 mini kernel 作为"二次引导程序"的所有准备工作。ATA PIO 驱动能从磁盘读取任意扇区，ELF 解析器能验证和加载 ELF64 二进制文件的 PT_LOAD 段，big kernel loader 把两者串成了一条完整的管线。虽然大内核本身还不存在，但这套基础设施已经完全就绪——等后续 milestone 编译出一个大内核的 ELF 文件放到磁盘镜像的 LBA 848 处，mini kernel 就能把它读出来、解析好、跳转过去执行。

从架构的角度看，这种"mini kernel 当引导程序、大内核当真正操作系统"的两阶段设计在很多真实 OS 项目中都能看到。它的好处是 mini kernel 可以保持极小的体积（受限于 Bootloader 阶段的内存约束），而大内核不受此限制，可以包含完整的驱动栈和用户态支持。加载完成后 mini kernel 的代码和数据就不再需要了，大内核可以回收那部分内存。

下一步就是真正开始构建大内核了——那将是 Cinux 从一个"能启动的引导程序"进化为一个"真正的操作系统"的起点。不过在那之前，让我们先把现在的成果稳稳地跑通。

---

> 本章对应 milestone：`008_mini_kernel_disk_and_loader`
> 上一章：[007 - Mini Kernel 中断系统](007-mini-kernel-interrupts.md)
> 下一章预告：大内核构建与跳转（milestone 009+）
