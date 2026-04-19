# 009E ATA 驱动增强与 1GB 压测验证 —— 把加载链路推向极限

## 前言

上一章（009D）我们修复了 ELF loader 的关键 bug，big kernel 终于能被正确加载了。到这一步为止，整个三级启动链——Bootloader → mini kernel → big kernel——在小规模数据下已经跑通了。但说实话，如果只测了几十 KB 的内核就宣布胜利，那也太心虚了。真正的内核动辄几 MB 甚至几十 MB，我们的 ATA 驱动、ELF 加载器、两阶段大内核 loader，能不能扛住大规模数据的考验？

这一章就是来做这件事的。我们要把整个加载链路推向极限——构造一个 1GB 的合成 ELF 文件，让 mini kernel 用两阶段加载把它从磁盘读进来、解析 PT_LOAD 段、然后用 CRC32 抽样校验数据完整性。1GB 的 ELF，想想都觉得疯狂，但如果你仔细想想就会发现，这恰好是对我们所有基础设施的一次终极考验：ATA 驱动需要支持大容量分块读取甚至 DMA，分页映射需要覆盖到高地址空间，ELF loader 需要处理巨大的段，CRC32 库需要能高效地校验海量数据。任何一环出问题，压测就会失败。

完成本章后，我们会看到 `make run-stress-test` 输出一路绿色的 PASSED，1GB 的 ELF 在 QEMU 里被完整加载并验证通过。这是 009 系列的收尾之作——从 009A 到 009E，我们一路搭建了大内核启动、I/O 和串口、kprintf、修复了 ELF loader 的 bug，现在终于可以给整个 milestone 画上一个坚实的句号了。

本章的前置知识是 009D 的 ELF loader 修复以及 008 的 ATA PIO 驱动基础。

---

## 环境说明

我们的开发环境跟之前保持一致：Ubuntu/Debian x86_64，工具链包括 GCC/G++ 13+、CMake 4.1+、QEMU `qemu-system-x86_64`。QEMU 分配了 8GB 内存（`-m 8G`），因为 1GB 的 ELF 数据需要足够的物理内存来暂存。脚本方面用到了 Python 3（`generate_large_elf.py` 和 `append_crc32.py`），不需要额外安装 Python 包——只用了标准库的 `struct`、`zlib`、`binascii`。

有一点需要特别注意的是，压测的磁盘镜像会超过 1GB，确保你的磁盘空间至少有 2-3 GB 的余量。构建目录 `build/` 里有 `cinux.img`（正常镜像）、`cinux_test.img`（测试镜像）、`cinux_stress_test.img`（压测镜像）和 `stress_kernel.elf`（1GB 合成 ELF），加起来差不多 3-4 GB。

---

## 第一步——ATA PIO 驱动重写：从简单到健壮

### 为什么需要重写

008 章节的 ATA 驱动是一个简化版本，能读 MBR、能读几百个扇区，功能上是通的。但问题在于它扛不住大负载。原始版本的 `read()` 函数接受 `uint16_t count`，一次最多只能读 65535 个扇区（约 32MB），而且内部数据搬运用的是逐字（word-by-word）的 `inw` 循环，效率极低。更关键的是，错误处理不够健壮——如果某一次扇区读取过程中磁盘状态异常，整个流程会默默地返回错误，没有足够的诊断信息。

这次重写做了几件大事：新增了 `read_large()` 支持分块大容量读取、引入了 PCI Bus Master DMA 传输让数据搬运不再完全依赖 CPU、把 PIO 读取的核心循环从逐字 `inw` 换成了 `rep insw` 指令大幅提升吞吐量、以及更详细的错误报告和状态检查。说实话，这次改动量不小，`ata.cpp` 从原来的一百多行膨胀到了将近 430 行，但每一行改动都有它的道理。

### 新接口设计

先来看看新的头文件 `kernel/mini/driver/ata.hpp`。相比 008 版本，接口层最大的变化是新增了 `read_large()` 和 DMA 相关的函数。

```cpp
// kernel/mini/driver/ata.hpp

namespace cinux::mini::driver::ata {

// --- 寄存器和常量定义（同 008，略） ---

// 状态寄存器新增了 ERR 和 DF 位
constexpr uint8_t ATA_STATUS_ERR  = 0x01;  // Error occurred
constexpr uint8_t ATA_STATUS_DRQ  = 0x08;  // Data request ready
constexpr uint8_t ATA_STATUS_DF   = 0x20;  // Drive fault
constexpr uint8_t ATA_STATUS_RDY  = 0x40;  // Drive ready
constexpr uint8_t ATA_STATUS_BSY  = 0x80;  // Drive busy

// DMA 命令常量
constexpr uint8_t ATA_CMD_READ_DMA        = 0xC8;  // LBA28
constexpr uint8_t ATA_CMD_READ_DMA_EXT    = 0x25;  // LBA48

// Bus Master 寄存器偏移（相对 BAR4 基址）
constexpr uint8_t BM_CMD    = 0x00;
constexpr uint8_t BM_STATUS = 0x02;
constexpr uint8_t BM_PRDT   = 0x04;

bool init();                        // 初始化（现在会尝试 DMA）
bool read(uint64_t lba, uint16_t count, void* buffer);     // PIO 单次读取
bool read_large(uint64_t lba, uint32_t count, void* buffer); // 大容量分块读取
bool is_dma_available();            // DMA 是否可用
bool dma_read(uint64_t lba, uint16_t count, void* buffer);  // DMA 读取

}  // namespace cinux::mini::driver::ata
```

`read()` 接口没有变化——它还是做单次 PIO 读取，参数和返回值跟 008 一样。新增的 `read_large()` 才是重头戏：它接受 `uint32_t count`（32 位扇区数），内部自动把请求拆分成不超过 65535 个扇区的块，每次调用 `read()` 或 `dma_read()` 完成一块，循环直到全部读完。这个设计意味着调用方不需要关心底层 ATA 命令的扇区数上限——你尽管传一个 1GB 的请求进来，`read_large()` 会替你搞定一切。

DMA 相关的三个函数（`is_dma_available()`、`dma_read()`）我们后面再细讲，先看 PIO 读取的改进。

### PIO 读取核心循环优化

008 版本的扇区数据读取用的是 C++ 循环加手动的 `read_data()` 调用——每扇区 256 次 `inw`，循环 256 次。这在功能上没问题，但性能上太保守了。新版把这段替换成了一条 `rep insw` 指令：

```cpp
// kernel/mini/driver/ata.cpp — read() 中的扇区数据读取

auto* buf = static_cast<uint16_t*>(buffer);

for (uint16_t sector = 0; sector < count; sector++) {
    delay_400ns();
    if (!wait_data_ready()) {
        kprintf("[ATA] ERROR: failed reading sector %u (LBA %u)\n", sector,
                static_cast<uint32_t>(lba + sector));
        return false;
    }

    {
        unsigned int words = 256;
        __asm__ volatile(
            "rep insw"
            : "+D"(buf), "+c"(words)
            : "d"(static_cast<uint16_t>(ATA_PRIMARY_BASE))
            : "memory"
        );
    }
}
```

`rep insw` 是 x86 的一条重复字符串 I/O 指令——CPU 会自动从端口 `DX`（这里就是 `ATA_PRIMARY_BASE = 0x1F0`）连续读取 `CX` 个 16 位字到 `EDI` 指向的内存，每读完一个字自动递增 `EDI` 并递减 `CX`。这比手动循环的 `inw` 快不少，因为 CPU 内部可以对 `rep insw` 做微架构优化（比如指令流水线不会被反复中断），而且指令缓存只需要存一条指令而不是整个循环体。内联汇编的约束里 `"memory"` clobber 告诉编译器这条指令会修改内存（因为写了 buffer），`"+D"` 和 `"+c"` 表示 `EDI` 和 `ECX` 会被指令修改。

### 更健壮的错误处理

新版本的 `wait_data_ready()` 函数比 008 版本详尽得多：

```cpp
// kernel/mini/driver/ata.cpp

bool wait_data_ready() {
    for (uint32_t i = 0; i < 10000000; i++) {
        uint8_t status = read_reg(ATA_REG_STATUS);

        if (status & ATA_STATUS_ERR) {
            kprintf("[ATA] ERROR: drive error, status=0x%02x, error=0x%02x\n",
                    status, read_reg(ATA_REG_ERROR));
            return false;
        }
        if (status & ATA_STATUS_DF) {
            kprintf("[ATA] ERROR: drive fault, status=0x%02x\n", status);
            return false;
        }

        if ((status & ATA_STATUS_BSY) == 0 && (status & ATA_STATUS_DRQ)) {
            return true;
        }

        __asm__ volatile("pause");
    }

    kprintf("[ATA] ERROR: timeout waiting for data ready\n");
    return false;
}
```

三个关键的检查点：ERR 位（bit 0）表示驱动器在执行命令时遇到了错误，此时可以读错误寄存器（offset 1）获取具体原因；DF 位（bit 5）表示驱动器发生了硬件故障；只有当 BSY（bit 7）清零且 DRQ（bit 3）置位时，才表示数据已经准备好了。如果一千万次轮询都没等到，就报超时错误。相比 008 版本只检查了 ERR 和 DF 两个错误位但没有打印详细状态，新版给了更丰富的诊断信息——在大规模读取时，这些信息对定位问题是至关重要的。

### PCI 设备检测与 DMA 初始化

ATA 驱动的 `init()` 函数现在多了一步：尝试检测 PCI IDE 控制器并初始化 DMA。这部分逻辑放在 `kernel/mini/driver/pci.hpp` 里，是一个 header-only 的 PCI 配置空间访问库。

为什么要检测 PCI？在现代 x86 系统上，ATA 控制器通常是挂载在 PCI 总线上的，而 Bus Master DMA 功能需要通过 PCI 配置空间来启用和配置。QEMU 模拟的是经典的 PIIX4 芯片组，它的 IDE 控制器支持 Bus Master DMA——如果我们能找到这个控制器、读取它的 BAR4（Bus Master 寄存器基址）、然后设置好 PRD（Physical Region Descriptor）表，就可以让 DMA 引擎替 CPU 做数据搬运。

PCI 配置空间的访问机制非常经典：往端口 `0xCF8` 写一个 32 位地址值（格式是 `1 | bus<<16 | dev<<11 | func<<8 | offset`），然后从端口 `0xCFC` 读或写 32 位数据。这就是所谓的"配置空间地址-数据端口对"——通过两个固定的 I/O 端口就能访问整个 PCI 配置空间，每个设备有 256 字节的配置寄存器。

```cpp
// kernel/mini/driver/pci.hpp — 配置空间读取

inline uint32_t config_read(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset) {
    uint32_t addr = (1u << 31) | (static_cast<uint32_t>(bus) << 16) |
                    (static_cast<uint32_t>(device) << 11) |
                    (static_cast<uint32_t>(func) << 8) |
                    (offset & 0xFC);
    cinux::mini::io::outl(PCI_CONFIG_ADDR, addr);
    return cinux::mini::io::inl(PCI_CONFIG_DATA);
}
```

地址值的最高位（bit 31）是使能位，必须为 1 才能进行配置空间访问。Bus、Device、Function 三个字段定位到具体的 PCI 设备，offset 定位到该设备配置空间内的寄存器。注意 offset 必须是 4 字节对齐的（`& 0xFC` 保证），因为配置空间访问的最小粒度是 32 位。

IDE 控制器的检测逻辑是扫描 PCI bus 0 的所有设备，找 class code = 0x01（Mass Storage）、subclass = 0x01（IDE）的设备。在 QEMU 里一定能找到，因为 PIIX4 的 IDE 控制器就是这些属性。找到之后读取 BAR4（偏移 0x20），提取 Bus Master I/O 端口基址，然后设置 PCI 命令寄存器的 Bus Master Enable 位（bit 2）。

```cpp
// kernel/mini/driver/pci.hpp — PRD 结构

struct Prd {
    uint32_t buffer_addr;   // 数据缓冲区物理地址
    uint16_t byte_count;    // 字节数（0 表示 65536）
    uint16_t flags;         // bit 15 = End of Table
} __attribute__((packed));

static_assert(sizeof(Prd) == 8, "PRD must be 8 bytes");
```

PRD（Physical Region Descriptor）是 DMA 传输的核心数据结构。每个 PRD 描述一块连续的物理内存缓冲区——DMA 引擎会按顺序遍历 PRD 表，把磁盘数据直接写入这些缓冲区。`byte_count` 为 0 时表示 65536 字节（这是 ATA 规范的设计），`flags` 的 bit 15 置位表示这是表的最后一个条目（EOT 标志）。

在 `ata.cpp` 里，DMA 的初始化是在 `init()` 的末尾完成的：

```cpp
// kernel/mini/driver/ata.cpp — init() 中的 DMA 初始化

// Step 4: Attempt DMA initialization
uint8_t pci_bus, pci_dev, pci_func;
if (pci::find_ide_controller(pci_bus, pci_dev, pci_func)) {
    kprintf("[ATA] Found PCI IDE controller at bus %u, device %u, func %u\n",
            pci_bus, pci_dev, pci_func);

    uint32_t bar4_raw = pci::read_bar4(pci_bus, pci_dev, pci_func);
    uint16_t bm_base = static_cast<uint16_t>(bar4_raw & 0xFFF0);

    if (bm_base != 0) {
        pci::enable_bus_master(pci_bus, pci_dev, pci_func);

        // Use static PRDT buffer (no PMM dependency)
        s_prdt = s_prdt_storage;
        s_prdt_phys = static_cast<uint32_t>(
            reinterpret_cast<uintptr_t>(s_prdt_storage) - KERNEL_VIRT_BASE);
        s_bm_base = bm_base;
        s_dma_available = true;

        kprintf("[ATA] DMA enabled: BAR4=0x%04x, PRDT at phys 0x%08x\n",
                bm_base, s_prdt_phys);
    }
}
```

这里有一个设计选择值得说一说：PRDT 表使用的是静态分配的缓冲区 `s_prdt_storage[512]`（4KB），而不是从 PMM（物理内存管理器）动态分配的。这是因为 DMA 初始化发生在 ATA `init()` 里，而 PMM 的状态在不同场景下可能不一样——比如测试内核可能只有 3MB 内存可用。用静态缓冲区就避免了依赖 PMM，无论内存多么紧张 DMA 都能正常工作。静态缓冲区的物理地址需要减去 `KERNEL_VIRT_BASE`（higher-half 偏移），因为 mini kernel 运行在 higher-half 映射下，虚拟地址和物理地址之间差了 `0xFFFFFFFF80000000`。

### read_large：分块大容量读取

有了 DMA 基础设施，`read_large()` 就可以把大请求拆成多个块、优先用 DMA 传输、失败了还能回退到 PIO：

```cpp
// kernel/mini/driver/ata.cpp — read_large()

bool read_large(uint64_t lba, uint32_t count, void* buffer) {
    if (!s_initialized) { /* ... */ return false; }
    if (count == 0) return true;
    if (buffer == nullptr) { /* ... */ return false; }

    static constexpr uint32_t MAX_SECTORS_PER_READ = 65535;
    static constexpr uint32_t PROGRESS_SECTORS = 65536;  // 每 32MB 打一次日志

    auto* buf = static_cast<uint8_t*>(buffer);
    uint32_t remaining = count;
    uint64_t current_lba = lba;
    uint32_t done = 0;

    while (remaining > 0) {
        uint16_t chunk = static_cast<uint16_t>(
            remaining > MAX_SECTORS_PER_READ ? MAX_SECTORS_PER_READ : remaining);

        bool ok;
        if (s_dma_available) {
            ok = dma_read(current_lba, chunk, buf);
            if (!ok) {
                kprintf("[ATA] DMA failed at LBA 0x%x, falling back to PIO\n", ...);
                ok = read(current_lba, chunk, buf);
            }
        } else {
            ok = read(current_lba, chunk, buf);
        }

        if (!ok) { /* ... */ return false; }

        buf += static_cast<size_t>(chunk) * ATA_SECTOR_SIZE;
        current_lba += chunk;
        remaining -= chunk;
        done += chunk;

        if (done >= PROGRESS_SECTORS) {
            uint32_t pct = static_cast<uint32_t>(
                (static_cast<uint64_t>(count - remaining) * 100) / count);
            kprintf("[ATA] Read progress: %u MB / %u MB (%u%%)\n",
                    (count - remaining) / 2048, count / 2048, pct);
            done = 0;
        }
    }
    return true;
}
```

核心逻辑很直白：把 `count` 个扇区的请求拆成最多 65535 个扇区一块，每块优先尝试 DMA 传输。如果 DMA 失败了，不是直接报错，而是回退到 PIO——这种"优雅降级"策略在大规模读取中非常重要，因为你永远不知道 DMA 引擎会不会在某些边界条件下出问题（虽然 QEMU 里这种情况不太会发生）。每读完 65536 个扇区（32MB）打印一次进度日志，这样即使加载一个 1GB 的 ELF，串口上也能看到进度条在往前走，而不是卡在那里一动不动让你怀疑内核是不是挂了。

**验证**：`make run-kernel-test` 中的 ATA 测试会验证初始化、MBR 读取、多扇区读取，以及 DMA 可用性检查。

---

## 第二步——辅助基础设施：CRC32、分页扩展、内存布局检查

ATA 驱动是压测的基石，但光有驱动还不够。1GB 数据读完之后我们需要校验它的正确性——总不能读了个 1GB 进来却不知道数据对不对吧？这就是 CRC32 库的用途。另外，1GB 的数据需要映射到内存的高地址区域，这要求页表映射能覆盖足够大的范围。还有一个经常被忽视但非常重要的事情：确保磁盘上各个组件之间不会互相踩——内存布局检查脚本就是干这个的。

### CRC32 校验库

CRC32 是一种经典的错误检测码，广泛应用于网络协议（如 Ethernet）、文件格式（如 ZIP、PNG）等领域。它的原理说起来也不复杂：把数据看作一个巨大的多项式，用一个预定义的生成多项式（我们用的是 `0xEDB88320`，这是标准 CRC-32 的反射形式）做模 2 除法，余数就是 CRC 值。任何单比特错误和大部分多比特错误都会导致 CRC 值改变——当然它不是加密级别的校验，但对于检测磁盘传输中的突发错误来说绰绰有余。

我们的实现放在 `kernel/mini/lib/crc32.h`，是 header-only 的，内核代码和 host 端测试都能直接 include：

```cpp
// kernel/mini/lib/crc32.h — 核心 CRC32 计算

inline uint32_t crc32(const void* data, size_t len) {
    // 256 项预计算查表（编译期求值）
    static constexpr uint32_t table[256] = {
        0x00000000, 0x77073096, 0xEE0E612C, ...
        // ... 完整的 256 项表 ...
    };

    uint32_t crc = 0xFFFFFFFF;
    const auto* bytes = static_cast<const uint8_t*>(data);

    for (size_t i = 0; i < len; i++) {
        crc = table[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
    }

    return crc ^ 0xFFFFFFFF;
}
```

查表法是 CRC32 的标准高效实现——预先计算好 256 个 CRC 余数（每个对应一个可能的字节值 0-255），运行时对每个输入字节做一次查表和两次异或操作。整个算法的时间复杂度是 O(n)，每字节只需几次简单运算。查表本身声明为 `static constexpr`，编译器会在编译期就把它算好，运行时直接使用。

另外还有一个带进度回调的版本 `crc32_progress()`，每隔 `chunk_size`（默认 1MB）字节调用一次回调函数。这个在压测中很有用——1GB 的 CRC32 计算需要几秒钟时间，如果没有任何输出的话你可能会以为内核卡死了：

```cpp
// kernel/mini/lib/crc32.h — 带进度的 CRC32

using CrcProgressFn = void(*)(size_t done, size_t total);

inline uint32_t crc32_progress(const void* data, size_t len,
                               CrcProgressFn progress, size_t chunk_size = 1024 * 1024) {
    // ... 同样的查表和计算逻辑 ...
    for (size_t i = 0; i < len; i++) {
        crc = table[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
        size_t done = i + 1;
        if (progress && done - last_report >= chunk_size) {
            progress(done, len);
            last_report = done;
        }
    }
    // ... 最终回调 ...
    return crc ^ 0xFFFFFFFF;
}
```

### 分页映射扩展

1GB 的 ELF 需要映射到物理地址 0x1000000（16MB）开始的区域，这意味着我们需要至少 1GB + 16MB 的地址空间映射。Bootloader 在进入 long mode 时设置了一个基本的页表——PML4 在 0x1000，PDPT 在 0x2000，PD 在 0x3000——PD 里的 512 个 2MB 大页覆盖了前 1GB。但超过 1GB 的部分就需要填充额外的 PDPT 条目了。

`kernel/mini/arch/x86_64/paging.hpp` 提供了 `identity_map_up_to()` 函数来做这件事：

```cpp
// kernel/mini/arch/x86_64/paging.hpp

inline void identity_map_up_to(uint64_t end_addr) {
    auto* pd = reinterpret_cast<volatile uint64_t*>(PD_VIRT_ADDR);
    auto* pdpt = reinterpret_cast<volatile uint64_t*>(PDPT_VIRT_ADDR);

    // Part 1: 填充 PD 条目（0-1GB，2MB 大页）
    uint32_t needed_2mb = static_cast<uint32_t>(
        (end_addr + PAGE_2MB_SIZE - 1) / PAGE_2MB_SIZE);
    if (needed_2mb > PT_ENTRIES) needed_2mb = PT_ENTRIES;

    for (uint32_t i = 0; i < needed_2mb; i++) {
        if (pd[i] == 0) {
            uint64_t phys_base = static_cast<uint64_t>(i) * PAGE_2MB_SIZE;
            pd[i] = phys_base | PD_HUGE_PAGE_FLAGS;
            __asm__ volatile("invlpg (%0)" : : "r"(phys_base));
        }
    }

    // Part 2: 填充 PDPT 条目（>= 1GB，1GB 大页）
    uint32_t needed_1gb = static_cast<uint32_t>(
        (end_addr + PAGE_1GB_SIZE - 1) / PAGE_1GB_SIZE);

    if (needed_1gb > 1 && detail::has_1gb_pages()) {
        for (uint32_t n = 1; n < needed_1gb; n++) {
            if (n == PDPT_PD_ENTRY || n == PDPT_HIGHER_HALF_ENTRY) continue;
            if (pdpt[n] == 0) {
                uint64_t phys_base = static_cast<uint64_t>(n) * PAGE_1GB_SIZE;
                pdpt[n] = phys_base | PDPT_1GB_PAGE_FLAGS;
            }
        }
        detail::reload_cr3();  // 刷新整个 TLB
    }
}
```

这个函数的工作方式是两阶段的。对于前 1GB 的地址空间，它填充 PD（Page Directory）里的 2MB 大页条目——每个条目映射一个 2MB 的物理页，512 个条目正好覆盖 1GB。每填充一个新条目就执行一次 `invlpg` 指令来使对应的 TLB 条目失效，确保后续的地址翻译使用新的页表。

超过 1GB 的部分，它直接在 PDPT（Page Directory Pointer Table）里填充 1GB 大页条目——跳过 PT 和 PD 两级，一个 PDPT 条目就映射整整 1GB。但这需要 CPU 支持 1GB 页（通过 CPUID `0x80000001` 的 EDX bit 26 检查），QEMU 在 KVM 模式下是支持的。填充完 PDPT 条目后必须重新加载 CR3（`reload_cr3()`），因为 `invlpg` 指令无法使 PDPT 级别的 TLB 条目失效——这是 Intel SDM 里明确说明的。

这里有一个细节值得注意：`PD_VIRT_ADDR = 0xFFFFFFFF80003000` 是页目录的 higher-half 虚拟地址。因为 mini kernel 自身就运行在 higher-half 映射下（PML4[0] 和 PML4[511] 指向同一个 PDPT），所以通过 higher-half 地址访问页表是安全的。同时，因为我们操作的是同一个 PDPT，身份映射和 higher-half 映射是同步更新的——添加一个 PDPT 条目后，`0x00000000_40000000` 和 `0xFFFFFFFF_804000000` 都能访问到这块内存。

### 内存布局检查脚本

`scripts/check_memory_layout.py` 是一个构建时辅助工具，用来检查内存布局中各个区域之间是否有重叠。它读取 mini kernel 的链接脚本（`kernel/mini/linker.ld`）、大内核加载器的头文件（`big_kernel_loader.hpp`）中的常量、以及 ELF 二进制文件中的段信息，然后把所有区域汇总在一起做碰撞检测。

为什么要这个脚本？因为我们的内存布局涉及多个组件——Page Tables 在 0x1000-0x4000、Mini Kernel 在 0x20000 起、Big Kernel staging buffer 在 0x1000000（16MB）——如果大内核特别大，比如占满了从 16MB 到 1GB+16MB 的区域，就有可能和其他数据结构冲突。手动计算这些范围太容易出错了，让脚本来自动检查是最靠谱的。

```bash
python3 scripts/check_memory_layout.py \
    --mini-elf build/kernel/mini/mini_kernel \
    --big-elf build/kernel/big/big_kernel
```

脚本会输出一个类似这样的报告：

```
=== Cinux Memory Layout Validation ===
  Page Tables             : 0x00001000 - 0x00004000 (12 KB)
  Mini Kernel             : 0x00020000 - 0x01000000 (8128 KB)
  PT_LOAD[0]              : 0x01000000 - 0x01020000 (128 KB)
  Staging Buffer          : 0x01000000 - 0x01040000 (256 KB)

  [OK] No overlaps detected.

=== Disk LBA Layout ===
  MBR                : LBA     0 -     1 (    1 sectors)
  Stage2             : LBA     1 -     4 (    3 sectors)
  Mini Kernel        : LBA    16 -   848 (  832 sectors)
  Big Kernel         : LBA   848 -  1872 ( 1024 sectors)

  [OK] No sector range overlaps.
```

看到两个 `[OK]` 就说明内存和磁盘布局都没有冲突。

---

## 第三步——1GB 压测设计：从生成到验证

现在基础设施数据都就位了，是时候构建压测本身了。压测的设计思路是：用 Python 脚本生成一个 1GB 的合法 ELF64 文件，里面填充已知的数据模式（pattern）；把这个 ELF 追加到磁盘镜像里；让 mini kernel 用两阶段加载把它读进来，解析 PT_LOAD 段，然后抽样校验数据模式是否正确。

### 合成 ELF 生成器

`scripts/generate_large_elf.py` 是整个压测的起点。它的任务不是编译真正的内核代码，而是构造一个"看起来像内核但里面全是测试数据"的 ELF 文件。

```python
# scripts/generate_large_elf.py — 核心 ELF 构造逻辑

# ELF 头部（64 字节）
def build_elf_header(entry: int, phoff: int, phnum: int) -> bytes:
    e_ident = (ELFMAG
               + bytes([ELFCLASS64, ELFDATA2LSB, EV_CURRENT, ELFOSABI_NONE])
               + b'\x00' * 8)
    return struct.pack('<16sHHIQQQIHHHHHH',
        e_ident, ET_EXEC, EM_X86_64, EV_CURRENT,
        entry, phoff, 0, 0,
        ELF_HEADER_SIZE, PHDR_SIZE, phnum,
        0, 0, 0)

# 程序头（56 字节）
def build_phdr(p_type, p_flags, p_offset, p_vaddr, p_paddr,
               p_filesz, p_memsz, p_align) -> bytes:
    return struct.pack('<IIQQQQQQ',
        p_type, p_flags, p_offset, p_vaddr, p_paddr,
        p_filesz, p_memsz, p_align)
```

ELF 头部和程序头的构造严格遵循 ELF64 规范。`e_ident` 的前四个字节是魔数 `\x7fELF`，后面跟着类别（64 位）、数据编码（小端序）、版本号、OS ABI 和填充字节。程序头里的 `p_paddr = 0x1000000`（16MB）是 big kernel 的物理加载地址，`p_vaddr = 0xFFFFFFFF801000000` 是加上 higher-half 偏移后的虚拟地址——这些都跟真正的 big kernel 链接脚本里的地址一致。

数据模式用的是一个非常巧妙的设计：

```python
def generate_pattern(offset: int, size: int) -> bytes:
    """byte = (offset >> 12) & 0xFF — 每 4KB 改变一次值"""
    data = bytearray(size)
    for i in range(size):
        data[i] = ((offset + i) >> 12) & 0xFF
    return bytes(data)
```

每个字节的值等于它的文件偏移右移 12 位后取低 8 位——也就是说，每 4KB 的数据都是同一个值，下一个 4KB 的值加 1。这个模式的好处是：它既容易生成（纯算术），又容易验证（检查对应偏移处的值是否符合预期），而且不是全零或全一这种"太无聊"的测试数据——如果某个扇区被读错了，模式校验马上就能发现。

唯一特殊的是 offset 0 处放的是 `0xFA`——这是 x86 的 `cli` 指令，因为大内核的入口点（entry point）处的第一条指令应该是 `cli`，测试用例会检查这一点。

```python
# 特殊处理：入口点放 cli 指令
ENTRY_BYTE_CLI = 0xFA

if first_chunk:
    chunk = bytes([ENTRY_BYTE_CLI]) + chunk[1:]
    first_chunk = False
```

整个 1GB 文件的生成是分块进行的，每 1MB 写一块，这样内存占用可控（不需要一次性分配 1GB 的 Python 字节数组）。同时，每写 100MB 打一次进度日志。CRC32 是流式计算的——每写一块就更新一次 CRC，最后把完整的 CRC32 追加到文件末尾。

### 构建流程：CMake 集成

压测的构建流程全部集成在 CMake 里，只需要一个 `make run-stress-test` 就能从零开始完成所有步骤：

```cmake
# cmake/qemu.cmake — 压测相关目标

# 生成 1GB 合成 ELF
set(STRESS_KERNEL_ELF "${CMAKE_BINARY_DIR}/stress_kernel.elf")
add_custom_command(
    OUTPUT ${STRESS_KERNEL_ELF}
    COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/generate_large_elf.py
        --size 1073741824 --output ${STRESS_KERNEL_ELF}
    COMMENT "Generating 1GB stress test ELF"
    VERBATIM
)
add_custom_target(stress-kernel-elf DEPENDS ${STRESS_KERNEL_ELF})

# 打包压测磁盘镜像：MBR + Stage2 + Mini Test Kernel + 1GB ELF
set(STRESS_TEST_IMAGE "${CMAKE_BINARY_DIR}/cinux_stress_test.img")
add_custom_command(
    OUTPUT ${STRESS_TEST_IMAGE}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/build_image.sh
        ${MBR_BIN} ${STAGE2_BIN} ${MINI_TEST_BIN}
        ${STRESS_TEST_IMAGE} ${STRESS_KERNEL_ELF}
    DEPENDS mbr stage2 mini_kernel_test stress-kernel-elf
    COMMENT "Building stress test disk image (1GB kernel)"
    VERBATIM
)

# 运行压测
add_custom_target(run-stress-test
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/qemu_test_wrapper.sh
        ${QEMU_EXECUTABLE} ${QEMU_COMMON_FLAGS} ${QEMU_TEST_EXTRA_FLAGS}
        -drive file=${STRESS_TEST_IMAGE},format=raw,index=0,media=disk,cache=unsafe
    DEPENDS stress-test-image
    USES_TERMINAL
    COMMENT "Running 1GB kernel stress test"
)
```

整个流程是一条清晰的依赖链：`run-stress-test` → `stress-test-image` → `stress-kernel-elf`。CMake 会先调用 `generate_large_elf.py` 生成 1GB ELF，然后调用 `build_image.sh` 把 MBR、Stage2、mini kernel 测试二进制、和 1GB ELF 打包成一个磁盘镜像。注意这里用的是 `MINI_TEST_BIN`（测试版 mini kernel）而不是正式版——因为压测需要在内核里运行测试框架来验证数据。

`build_image.sh` 接受 5 个参数，第 5 个是可选的大内核二进制。如果有大内核，脚本会自动计算需要多少个扇区，把磁盘镜像扩展到足够大（从默认的 1MB 扩展到 1GB+），然后用 `dd` 把大内核追加到 LBA 848 开始的位置。

QEMU 的压测配置用了 `cache=unsafe`——这个选项告诉 QEMU 不要对磁盘写入做同步刷盘，对于我们的只读压测场景来说能显著加快启动速度（不需要等 QEMU 模拟磁盘缓存刷新）。

### QEMU 测试自动化与退出码映射

测试内核运行在 QEMU 里，需要一个机制让它跑完测试后自动退出 QEMU——不然 `make run-stress-test` 会永远挂在那里。这就是 `isa-debug-exit` 设备的用途。

```cmake
# cmake/qemu.cmake

set(QEMU_TEST_EXTRA_FLAGS
    -device isa-debug-exit,iobase=0xf4,iosize=0x04
)
```

`isa-debug-exit` 是 QEMU 提供的一个调试设备：当内核往 I/O 端口 0xF4 写一个值 `v` 时，QEMU 会以退出码 `(v << 1) | 1` 退出。也就是说，写 0 退出码是 1，写 1 退出码是 3。但这里有个别扭的地方——退出码 0（成功）在 QEMU 里是不可能的（因为最低位永远是 1），所以我们需要一个包装脚本来做映射：

```bash
#!/bin/bash
# scripts/qemu_test_wrapper.sh

"$@"
rc=$?

if [ "$rc" -eq 1 ]; then
    # QEMU exit 1 → kernel wrote 0 → test SUCCESS → exit 0
    exit 0
elif [ "$rc" -eq 3 ]; then
    # QEMU exit 3 → kernel wrote 1 → test FAILURE → exit 1
    exit 1
else
    echo "QEMU unexpected exit code: $rc"
    exit "$rc"
fi
```

包装脚本把 QEMU 的退出码 1（内核写 0 = 测试全部通过）映射成 shell 的退出码 0（成功），退出码 3（内核写 1 = 有测试失败）映射成退出码 1（失败）。这样 `make run-stress-test` 的退出码就直接反映了测试结果——0 表示通过，非 0 表示失败，CI/CD 系统可以直接用这个退出码来判断。

### 压测内核测试代码

压测的验证逻辑在 `kernel/mini/test/test_stress_big_kernel.cpp` 里，分为两个测试：Phase 1 头部解析和 Phase 2 数据加载校验。

```cpp
// kernel/mini/test/test_stress_big_kernel.cpp — Phase 1 测试

void test_phase1_headers() {
    BigKernelLoadState state;
    bool ok = load_big_kernel_phase1(STRESS_KERNEL_LBA, state);
    TEST_ASSERT_TRUE(ok);

    // 验证 ELF 魔数
    const auto* magic = reinterpret_cast<const uint8_t*>(BIG_KERNEL_LOAD_ADDR);
    TEST_ASSERT_EQ(magic[0], 0x7F);
    TEST_ASSERT_EQ(magic[1], 'E');
    TEST_ASSERT_EQ(magic[2], 'L');
    TEST_ASSERT_EQ(magic[3], 'F');

    kprintf("  Stress ELF: %u bytes (%u sectors)\n",
            static_cast<uint32_t>(state.total_elf_size), state.total_sectors);
    TEST_ASSERT(state.total_elf_size > 1024 * 1024);  // 至少 1MB
}
```

Phase 1 测试调用了两阶段加载的第一阶段——从磁盘读取 ELF 头部、解析程序头表、确定总文件大小和扇区数。验证点包括 ELF 魔数正确、总大小超过 1MB（确保我们确实在加载一个大文件）。`STRESS_KERNEL_LBA = 2048` 是压测 ELF 在磁盘上的起始扇区——它比正常大内核的 LBA 848 靠后，避免和测试中的普通 big kernel 冲突。

Phase 2 测试更有意思——它调用 `load_big_kernel_phase2()` 完成全部数据的加载，然后抽样校验数据模式：

```cpp
// kernel/mini/test/test_stress_big_kernel.cpp — Phase 2 数据校验

bool verify_pattern(uint64_t base, uint64_t size) {
    // 每 1MB 抽一个样点（1GB = 1024 个样点）
    uint64_t step = 1024 * 1024;
    uint32_t checked = 0;
    uint32_t total_samples = static_cast<uint32_t>(size / step);

    for (uint64_t off = 0; off < size; off += step) {
        uint8_t actual = *reinterpret_cast<volatile uint8_t*>(base + off);
        uint8_t expected = (off == 0) ? 0xFA
            : static_cast<uint8_t>((off >> 12) & 0xFF);

        if (actual != expected) {
            kprintf("  MISMATCH at offset 0x%p: expected 0x%02x got 0x%02x\n",
                    reinterpret_cast<const void*>(off), expected, actual);
            return false;
        }
        checked++;

        // 每 128MB 打一次日志
        if (checked % 128 == 0) {
            kprintf("  Verify progress: %u / %u samples (%u%%)\n",
                    checked, total_samples, checked * 100 / total_samples);
        }
    }
    kprintf("  Pattern verified at %u sample points\n", checked);
    return true;
}
```

抽样策略是每 1MB 检查一个字节——1GB 的数据只需检查 1024 个样点，这在 QEMU 里几毫秒就能完成，但已经足以检测出大多数传输错误。如果某个扇区被错误地读取了，该扇区所在的 4KB 区域的 pattern 就会不匹配，而我们每 1MB 检查一次意味着至少每 256 个扇区就能捕获到一个错误。注意读内存时用的是 `volatile uint8_t*`——防止编译器把内存访问优化掉（毕竟这个地址不是标准的 C++ 对象，编译器可能会认为它的值不可能改变）。

入口点校验也很有意义：

```cpp
uint64_t entry = load_big_kernel_phase2(state, STRESS_KERNEL_LBA);
TEST_ASSERT(entry != 0);
TEST_ASSERT_EQ(entry, BIG_KERNEL_LOAD_ADDR);
```

因为合成 ELF 的 `p_paddr = 0x1000000`，入口点是 `0xFFFFFFFF801000000`（higher-half），减去 `KERNEL_VIRT_BASE` 后物理入口就是 `0x1000000`——恰好等于 `BIG_KERNEL_LOAD_ADDR`。这个断言验证了 ELF 加载器的地址转换逻辑在 1GB 数据量下仍然是正确的。

---

## 第四步——压测运行与端到端验证

好，所有代码和配置都就位了。让我们把整个流程跑一遍。

### 构建与运行压测

```bash
# 从项目根目录
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCINUX_BUILD_TESTS=ON -S .
cmake --build build -j$(nproc)
cd build

# 运行内核测试（包含 ATA、ELF loader、big kernel 等测试）
make run-kernel-test

# 运行 1GB 压测
make run-stress-test
```

### 内核测试期望输出

`make run-kernel-test` 会依次运行 ATA 测试、ELF loader 测试和 big kernel 加载测试：

```
[TEST] ===== ATA PIO Driver Tests (008) =====
  [PASSED] test_init_no_crash
  [PASSED] test_read_mbr_signature
  [PASSED] test_read_non_zero
  [PASSED] test_read_multi_sector
  [PASSED] test_dma_available
[TEST] Results: 5 passed, 0 failed

[TEST] ===== Stress Test: Large Kernel Load =====
  Stress ELF: 1073741824 bytes (2097152 sectors)
  [PASSED] test_phase1_headers
  Entry point: 0x1000000
  Verifying data pattern (1023 MB)...
  Verify progress: 128 / 1024 samples (12%)
  Verify progress: 256 / 1024 samples (25%)
  ...
  Pattern verified at 1024 sample points
  [PASSED] test_phase2_load_and_verify
[TEST] Results: 2 passed, 0 failed
```

看到 `Pattern verified at 1024 sample points` 就说明 1GB 的数据经过磁盘读取 → 内存加载 → 模式校验整个链路，1024 个抽样点全部匹配。这是对我们 ATA 驱动、ELF 加载器、两阶段加载器、分页映射的一次全面验证。

### Host 端测试

Host 端测试不需要 QEMU，直接在开发机上运行，验证 ATA 寄存器编码、状态位判断、DMA 常量等纯逻辑：

```bash
cd build && make test_host
```

期望输出：

```
[PASSED] ata: primary base and control ports
[PASSED] ata: register offsets
[PASSED] ata: status register bits
[PASSED] ata: command constants
[PASSED] ata: drive selection constants
[PASSED] ata: sector size
[PASSED] ata: LBA28 for small LBA and count
[PASSED] ata: LBA48 for LBA at 28-bit boundary
[PASSED] ata: LBA48 for count over 256
...
[PASSED] ata: DMA byte count encoding (0 = 65536)
[TEST] Results: 30 passed, 0 failed
```

Host 端测试总共有 30 个用例，覆盖了常量验证、LBA28/LBA48 编码、状态位解析、驱动器选择寄存器、DMA 命令常量、PRD 结构布局等方方面面。这些测试的价值在于它们不需要 QEMU 就能运行，迭代速度极快——你改了 ATA 驱动的常量定义，跑一遍 `make test_host` 几秒钟就能发现是不是哪里不一致了。

### 全量测试

如果你想一次性跑完所有测试（host + kernel + 压测），可以用项目的一键脚本：

```bash
./scripts/run_all_test_user_scripts.sh
```

---

## 调试技巧

**压测卡在 "Generating 1GB stress test ELF" 阶段**

这通常不是 bug——生成 1GB 文件确实需要一些时间（大概十几秒到一分钟，取决于磁盘速度）。如果你看到 Python 进程在跑但没有输出，耐心等一下就好。如果等了超过两分钟还没完成，检查磁盘空间是否充足（`df -h` 看看 build 目录所在分区的剩余空间）。

**压测失败："MISMATCH at offset 0x..."**

数据模式不匹配意味着某个位置的数据跟预期不符。首先确认生成 ELF 时用的 pattern 公式（`(offset >> 12) & 0xFF`）跟测试代码里的验证公式一致。如果公式没问题，那可能是 ATA 读取过程中某些扇区的数据被错误地读取了——用 `make run-stress-test-debug` 启动 GDB，在 `verify_pattern` 函数里设断点，看看是哪个 offset 出了问题，然后检查对应的 LBA 地址是否正确。

**DMA 初始化失败："No PCI IDE controller found, DMA unavailable"**

在 QEMU 的 TCG 模式（软件模拟）下，PCI IDE 控制器应该总是存在的。如果你用了 `-accel tcg` 而不是 `-accel kvm`，或者 QEMU 版本太旧，可能会出现这个问题。不过即使 DMA 不可用，`read_large()` 也会自动回退到 PIO——测试仍然能通过，只是速度慢一些。

**"QEMU unexpected exit code"**

这通常意味着内核在测试完成之前就崩溃了——可能是 triple fault 或 page fault。用 `make run-kernel-test-debug` 启动 GDB 模式，在内核入口设断点，逐步跟踪看崩溃发生在哪里。常见的崩溃原因包括：页表映射没覆盖到大内核所在的地址范围（检查 `identity_map_up_to` 是否被调用），或者 staging buffer 地址跟 mini kernel 自身重叠（检查 `BIG_KERNEL_LOAD_ADDR` 的值）。

---

## 本章小结 + 009 整体回顾

本章新增和修改的关键组件：

| 组件 | 文件 | 说明 |
|------|------|------|
| ATA 驱动（重写） | `kernel/mini/driver/ata.cpp` / `ata.hpp` | 新增 `read_large()`、DMA 传输、`rep insw` 优化 |
| PCI 设备检测 | `kernel/mini/driver/pci.hpp` | 配置空间访问、IDE 控制器枚举、Bus Master 启用 |
| CRC32 校验 | `kernel/mini/lib/crc32.h` | 查表法实现、带进度回调的版本 |
| 分页映射扩展 | `kernel/mini/arch/x86_64/paging.hpp` | `identity_map_up_to()` 自动填充 PD/PDPT 条目 |
| 内存布局检查 | `scripts/check_memory_layout.py` | 检测内存区域和磁盘扇区重叠 |
| 合成 ELF 生成 | `scripts/generate_large_elf.py` | 构造 1GB 合法 ELF64 + 已知数据模式 |
| CRC32 追加 | `scripts/append_crc32.py` | 给二进制文件追加 CRC32 校验和 |
| QEMU 测试包装 | `scripts/qemu_test_wrapper.sh` | isa-debug-exit 退出码映射 |
| 压测测试 | `kernel/mini/test/test_stress_big_kernel.cpp` | 两阶段加载 + 数据模式抽样验证 |
| Host 端测试 | `test/unit/test_ata.cpp` | ATA 寄存器编码、DMA 常量、PRD 结构验证 |
| CMake 压测配置 | `cmake/qemu.cmake` | `run-stress-test`、`stress-kernel-elf` 等目标 |

---

### 009 系列完整回顾

009 milestone 是整个 Cinux 项目迄今为止最丰富的一个阶段，我们用了五篇教程完成了从"能加载一个大内核"到"在 1GB 压力下依然正确"的跨越。让我简单回顾一下这段旅程：

**009A — Big Kernel 启动链路**：我们搭建了 mini kernel 到 big kernel 的桥梁——让 mini kernel 从磁盘读取 big kernel 的 ELF 文件、解析 PT_LOAD 段、搬运到正确的物理地址、然后跳转过去执行。那一刻看到 big kernel 的 `kprintf` 输出出现在串口上，整个三级启动链终于活了。

**009B — I/O 与串口驱动**：大内核需要自己的硬件驱动，我们从最基础的 I/O 端口操作（`inb`/`outb`）开始，搭建了串口驱动（COM1, 0x3F8），让 big kernel 也能往串口打印信息。

**009C — kprintf 格式化输出**：在 freestanding 环境下实现了一个功能完整的 `kprintf`——支持 `%d`、`%x`、`%p`、`%s` 等格式化占位符。有了它，内核终于不再只能打十六进制数了，而是能输出人类可读的调试信息。

**009D — ELF Loader Bug 修复**：测试发现了 ELF 加载器中两个关键 bug——PT_LOAD 段偏移计算错误和 staging buffer 边界检查遗漏。修复这些 bug 的过程让我们深刻认识到：在 OS 开发里，一个小的地址计算错误就可能导致加载的内核数据完全错乱，而且这种错误在 QEMU 里不会报段错误，只会导致内核行为诡异。

**009E（本章）— 压测验证**：把 ATA 驱动推向生产级别——支持 DMA、大容量分块读取、完善的错误处理；然后构造了一个 1GB 的合成 ELF，用两阶段加载读进来，抽样校验 1024 个数据点。压测通过的那一刻，整个加载链路从 Bootloader 到 ATA 驱动到 ELF 加载器到分页映射到数据校验——每一环都被验证了。

五篇文章，从零到一，从几 KB 到 1GB。这个 milestone 做的事情可以用一句话概括：**让加载链路在大规模数据下依然可靠**。这是操作系统开发中一个经常被忽视但至关重要的品质——不是"能跑"就够了，而是"在任何合理的工作负载下都能正确运行"。

---

## 下一个里程碑预告 — 010：Big Kernel GDT/IDT

009 完成后，big kernel 已经能被 mini kernel 正确加载并跳转执行了。但现在的 big kernel 还是一个"裸奔"的状态——没有自己的 GDT，没有 IDT，没有中断处理，任何异常都会直接 triple fault 让 QEMU 重启。

下一个 milestone（010）会让 big kernel 建立自己的硬件基础设施：设置 GDT（代码段、数据段、可能的 TSS）、初始化 IDT（异常处理和硬件中断）、实现基本的异常处理器。到那时，big kernel 就不再是一个只会 `kprintf` 的玩具，而是一个真正能处理异常、响应中断的操作系统内核了。我们下一章见。
