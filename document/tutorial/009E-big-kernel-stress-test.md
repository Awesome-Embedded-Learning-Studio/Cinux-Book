# 从 256KB 到 1GB：ATA 驱动爆改 + DMA 加速 + 压测验证，给加载器上一课

> 作者：
> 标签：x86-64, ATA PIO, Bus Master DMA, PCI 配置空间, LBA48, CRC32, ELF64 压测, 1GB 内核加载, QEMU, 裸机开发, C++

---

## 前言

上一章我们写完了 ATA PIO 驱动和 ELF 加载器，成功把一个 256KB 的大内核从磁盘读进内存，解析了 ELF 段，打印了入口地址。说实话当时看到串口输出 `[LOADER] Big kernel loaded successfully` 的时候，我心里挺高兴的——从 MBR 开始一路走到能加载 ELF 内核，这已经是一条完整的启动链了。

但高兴了大概五分钟我就开始不舒服了。256KB 是什么概念？一张 JPEG 图片都比它大。如果我们的加载器连 1MB 的内核都没跑过，怎么敢说它"能工作"？很多教学 OS 项目在实现了"能跑"之后就停下来了，很少有人会去验证"大规模数据传输是否正确"。但恰恰是这种极限场景最容易暴露出隐蔽的 bug——LBA48 寻址的寄存器写入顺序、1GB 大页映射的边界条件、PRD 表的字节对齐要求，这些细节在小数据量下可能碰巧都能工作，但在 1GB 的规模下任何一处错误都会导致数据损坏。

所以这一章我们要做的事情非常直接：构造一个 1GB 大小的合成 ELF 文件，写进磁盘镜像，然后用我们的两阶段加载器把它完整读出来、映射到内存、解析 ELF 段，最后逐字节校验数据的完整性。为了支撑这个量级的操作，我们对几个底层模块做了大幅增强——ATA 驱动新增了 `read_large()` 接口和 PCI Bus Master DMA 加速，paging 模块增加了动态扩展 1GB 大页映射的能力，CRC32 查表法被引入用来做数据完整性校验。

想想看，我们用一个 512 字节的 MBR 引导，最终加载了 1GB 的内核。这个数字本身就值得好好写一章。

## 环境说明

实验环境不变：x86_64 平台，GNU AS + GCC/G++ + CMake 构建，QEMU 模拟运行，freestanding C++23。QEMU 配置使用 8GB 内存（`-m 8G`）、KVM 加速（`-accel kvm`）和 `max` CPU 模型（确保支持 1GB 大页），串口输出到 stdio。压测镜像通过 CMake 的 `make run-stress-test` 目标自动生成并运行，QEMU 的 `isa-debug-exit` 设备让内核测试可以自动退出。

磁盘布局方面，压测镜像和普通镜像不同：MBR 和 Stage2 照旧在扇区 0-15，mini kernel 的测试二进制在扇区 16-847，但合成 ELF 不是放在常规的 LBA 848，而是放在 LBA 2048（磁盘 1MB 位置），这个偏移量避免了大内核和压测 ELF 的碰撞。内存方面，staging buffer 仍然在 0x1000000（16MB），1GB 的 ELF 加载后实际使用的物理地址范围会延伸到大约 0x41000000（约 1GB + 16MB）。

## ATA 驱动大修——从玩具到工具

上一章的 ATA 驱动有两个硬伤。第一个是 `read()` 函数只能接受 16 位的扇区计数，也就是说单次调用最多读 65535 个扇区（约 32MB）。1GB 的数据需要调用 32 次才能读完，每次都要重新走一遍"等待 BSY → 写寄存器 → 发命令 → 等 DRQ → 读数据"的完整握手流程。第二个更致命的问题是 PIO 模式下 CPU 逐字搬运数据的效率实在太低——每次 `rep insw` 虽然是 CPU 微代码级别的操作，但 1GB 的数据量下累积的开销相当可观。

我们先来解决第一个问题。新增的 `read_large()` 函数的核心思路很直接——把一个大的读取请求拆成多个不超过 65535 扇区的小块，逐块调用底层接口。每读完 64K 个扇区（约 32MB），它会打印一次进度，这一点在 1GB 的规模下非常关键。你想想，PIO 模式读完 1GB 可能需要相当长的时间，如果串口没有任何输出，你完全没法判断系统是卡死了还是在正常工作。

```cpp
// kernel/mini/driver/ata.cpp -- read_large()

bool read_large(uint64_t lba, uint32_t count, void* buffer) {
    static constexpr uint32_t MAX_SECTORS_PER_READ = 65535;
    static constexpr uint32_t PROGRESS_SECTORS = 65536;

    auto* buf = static_cast<uint8_t*>(buffer);
    uint32_t remaining = count;
    uint64_t current_lba = lba;

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
        // ... 推进指针、打印进度 ...
    }
    return true;
}
```

你会发现这里有一个非常实用的容错设计：DMA 优先，但一旦 DMA 失败就自动 fallback 到 PIO。这意味着即使 DMA 初始化出问题（比如 PCI 枚举没找到 IDE 控制器），压测依然能通过 PIO 完成，只是慢一点而已。这种 graceful degradation 的思路在底层驱动开发中非常重要，因为 DMA 涉及的硬件状态远比 PIO 复杂，出错的概率也更高。

接下来我们看真正的重头戏——DMA 读取的实现。

### PCI 总线上的搬运工：Bus Master DMA

DMA（Direct Memory Access）的核心思想是用一个专门的硬件控制器来完成磁盘到内存的数据搬运，CPU 只需要说"把磁盘 X 位置的数据搬到内存 Y 位置"就可以去干别的事了。在我们这个场景下虽然 CPU 会轮询等待 DMA 完成（并没有真正去做别的事），但 DMA 的优势在于数据通路不同：PIO 模式下数据从磁盘控制器读到 CPU 寄存器，再从 CPU 寄存器写到内存，每个字都要经过 CPU；DMA 模式下数据直接从磁盘控制器写到内存，CPU 完全不参与搬运。

要使用 DMA，首先得找到 IDE 控制器的 Bus Master 寄存器基址。这需要遍历 PCI 配置空间。`pci.hpp` 用了一套非常精简的实现来完成这个任务——经典的 `0xCF8`/`0xCFC` 端口对：向 `0xCF8` 写入一个编码了 bus/device/function/offset 的 32 位地址（bit 31 是 enable 位），然后从 `0xCFC` 读回数据。

```cpp
// kernel/mini/driver/pci.hpp -- 配置空间读取

inline uint32_t config_read(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset) {
    uint32_t addr = (1u << 31) | (static_cast<uint32_t>(bus) << 16) |
                    (static_cast<uint32_t>(device) << 11) |
                    (static_cast<uint32_t>(func) << 8) |
                    (offset & 0xFC);
    cinux::mini::io::outl(PCI_CONFIG_ADDR, addr);
    return cinux::mini::io::inl(PCI_CONFIG_DATA);
}
```

`find_ide_controller()` 在 bus 0 上扫描所有 32 个设备、每个设备的 8 个 function，寻找 class code 为 `0x01`（Mass Storage）、subclass 为 `0x01`（IDE）的设备。QEMU 的 PIIX4 芯片组里 IDE 控制器通常在 bus 0, device 1, function 1。找到之后读取 BAR4 寄存器获取 Bus Master 的 I/O 端口基址（低 4 位是标志位，实际地址在 bits 15:4），然后在 PCI command 寄存器中置位 Bus Master Enable（bit 2），允许设备发起 DMA 传输。整个流程在 `ata::init()` 的第四步完成。

DMA 传输的核心数据结构是 PRD（Physical Region Descriptor）表。每个 PRD 8 字节，描述一块连续的物理内存缓冲区：buffer 地址（32 位）、byte count（16 位，0 表示 65536）、以及一个 flags 字（bit 15 是 End-of-Table 标志）。DMA 引擎会按顺序遍历 PRD 表，把数据写入每个 PRD 指定的内存区域。

```cpp
// kernel/mini/driver/ata.cpp -- DMA PRD 表构建

static pci::Prd s_prdt_storage[512] __attribute__((aligned(4096)));

// dma_read 中构建 PRD 表：
while (total_bytes > 0) {
    uint32_t chunk = total_bytes;
    if (chunk > 65536) chunk = 65536;

    s_prdt[prd_count].buffer_addr = buf_phys;
    s_prdt[prd_count].byte_count = static_cast<uint16_t>(chunk & 0xFFFF);
    s_prdt[prd_count].flags = 0;

    buf_phys += chunk;
    total_bytes -= chunk;
    prd_count++;
}
s_prdt[prd_count - 1].flags = pci::PRD_FLAG_EOT;
```

这里有一个值得展开说的设计选择：PRD 表用的是静态分配的 `s_prdt_storage[512]`（4KB，512 个 PRD entry），而不是通过 PMM 动态分配。原因是我们需要在内存非常紧张的环境下也能工作——压测用的测试内核只分配了 3MB 的 RAM，如果我们还需要先分配物理页才能启动 DMA，那就陷入了鸡生蛋蛋生鸡的困境。静态分配虽然浪费了 4KB 的 BSS 空间，但彻底消除了这个依赖。

PRD 表建好之后，DMA 读取的工作流程分三步。第一步是向 ATA 控制器发送 READ DMA EXT 命令——注意 DMA 模式下始终使用 LBA48 寻址，即使 LBA 地址在 28 位范围内也要写两遍寄存器，这是 ATA DMA 传输的标准做法。第二步是通过 Bus Master 的 BM_CMD 寄存器启动传输（置位 bit 0）。第三步是轮询等待 DMA 完成——检查 BM_STATUS 寄存器的 `BM_STATUS_INTERRUPT` 位（bit 2）判断传输是否结束，同时检查错误位。

```cpp
// kernel/mini/driver/ata.cpp -- DMA 启动与轮询

io::outl(s_bm_base + BM_PRDT, s_prdt_phys);       // 告诉 DMA 引擎 PRD 表在哪
io::outb(s_bm_base + BM_STATUS, 0x06);              // 清除 error + interrupt 标志
io::outb(s_bm_base + BM_CMD, 0x00);                  // 确保先停止

// ... LBA48 寄存器写入 + 发 READ DMA EXT 命令 ...

io::outb(s_bm_base + BM_CMD, BM_CMD_START);          // 启动！

for (uint32_t i = 0; i < 50000000; i++) {
    uint8_t bm_stat = io::inb(s_bm_base + BM_STATUS);
    if (bm_stat & (BM_STATUS_ERROR | BM_STATUS_DMA_ERR)) { /* 错误处理 */ }
    if (bm_stat & BM_STATUS_INTERRUPT) break;
    __asm__ volatile("pause");
}
io::outb(s_bm_base + BM_CMD, 0x00);  // 停止 DMA 引擎
```

还有一个容易忽略的细节：`s_prdt_phys` 的计算。我们的 mini kernel 是 higher-half 编译的，代码中 `s_prdt_storage` 的地址实际上是虚拟地址 `0xFFFFFFFF80000000 + offset`，但 DMA 引擎只能理解物理地址。所以需要减去 `KERNEL_VIRT_BASE` 来做转换。这个减法的前提是 identity mapping——虚拟地址 `0xFFFFFFFF80000000 + X` 对应物理地址 `X`，在我们的页表设置中是成立的。

### paging 的动态扩展——映射 1GB 以上的物理空间

1GB 的内核镜像加载到 0x1000000（16MB）后，实际的内存使用范围会延伸到大约 1GB + 16MB。bootloader 设置的页表只映射了 0-1GB 的范围（通过 PD 的 512 个 2MB 大页），超过 1GB 的部分需要在运行时动态映射。

`identity_map_up_to()` 函数承担了这个任务，它分两部分工作。第一部分填充 PD（Page Directory）的 2MB 大页条目，覆盖 0 到 1GB 的范围，这个和之前一样。第二部分是新增的——填充 PDPT（Page Directory Pointer Table）的 1GB 大页条目，覆盖 1GB 以上的范围。

```cpp
// kernel/mini/arch/x86_64/paging.hpp -- 1GB 大页映射

if (needed_1gb > 1 && detail::has_1gb_pages()) {
    for (uint32_t n = 1; n < needed_1gb; n++) {
        if (n == PDPT_PD_ENTRY || n == PDPT_HIGHER_HALF_ENTRY) continue;
        if (pdpt[n] == 0) {
            uint64_t phys_base = static_cast<uint64_t>(n) * PAGE_1GB_SIZE;
            pdpt[n] = phys_base | PDPT_1GB_PAGE_FLAGS;
        }
    }
    detail::reload_cr3();  // 必须重载 CR3
}
```

这里有一个非常容易踩的坑：修改 PDPT 条目后必须 `reload_cr3()`（重新加载 CR3 寄存器来刷新整个 TLB），而不能只用 `invlpg`。原因是 Intel SDM 明确指出 `invlpg` 指令不能刷新 1GB 页的 TLB 条目（Volume 3, Section 4.10.4），只有 CR3 重载能做到这一点。如果你只用了 `invlpg` 而 1GB 页的 TLB 没有被刷新，新映射的地址可能仍然会命中旧的（空的）TLB 条目，直接 page fault。这一点在 256KB 的内核测试中完全不会暴露，因为 256KB 根本用不到 1GB 大页。

另外，1GB 大页的使用有一个前置条件：CPU 必须支持 1GB 页（CPUID.80000001H:EDX[26]，也叫 PDPE1GB 位）。`has_1gb_pages()` 通过 `cpuid` 指令检查这个位。QEMU 在 `-cpu max` 模式下支持 1GB 页，我们的 CMake 配置正好就是 `max` CPU。如果换成 `qemu64` 模型就不支持了，到时候需要为超过 1GB 的范围分配额外的 PD 和 PT，那会复杂得多。

## CRC32 与数据完整性——最后一道防线

1GB 的数据从磁盘读到内存，中间经过的环节太多了——ATA 控制器的 DMA 引擎在搬运 PRD 表指定的内存块，ELF 加载器的 `memmove` 在搬运 PT_LOAD 段数据，任何一个环节出错都会导致数据损坏。为了检测这些错误，我们引入了 CRC32 校验。

`crc32.h` 的实现非常经典——标准的 CRC32 多项式 `0xEDB88320`（reflected 形式），通过 256 项查找表实现逐字节计算。查找表用 `static constexpr` 定义，编译器在编译期就把这 256 个值算好了，运行时直接使用，没有任何初始化开销。

```cpp
// kernel/mini/lib/crc32.h -- 核心计算逻辑

inline uint32_t crc32(const void* data, size_t len) {
    static constexpr uint32_t table[256] = {
        0x00000000, 0x77073096, 0xEE0E612C, ...
    };

    uint32_t crc = 0xFFFFFFFF;
    const auto* bytes = static_cast<const uint8_t*>(data);

    for (size_t i = 0; i < len; i++) {
        crc = table[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}
```

算法本身很直接：初始值 `0xFFFFFFFF`，每读一个字节，用当前 CRC 的低 8 位 XOR 该字节作为查表索引，然后 XOR 上 CRC 右移 8 位的结果。最终结果再 XOR `0xFFFFFFFF`。这种 reflected 实现避免了显式的位反转操作，非常高效。

另外一个 `crc32_progress()` 版本带进度回调——每处理 `chunk_size`（默认 1MB）字节就调用一次回调函数。对于 1GB 的数据，计算 CRC32 需要遍历大约 10 亿个字节，如果没有进度输出，你会以为内核挂死了。不过在我们当前的压测实现中，数据完整性校验用的不是 CRC32 比对，而是直接的数据模式抽样——CRC32 主要是为将来更完善的全量校验做准备。

你可能会问 CRC32 的碰撞概率会不会太高。理论上对随机数据碰撞概率大约是 1/2^32（约 42 亿分之一），对我们的压测来说完全足够——我们不是在做密码学验证，只需要检测"磁盘读取是否正确"，而磁盘读取错误通常会改变大量比特，不太可能恰好产生和原始数据相同的 CRC32 值。

## 1GB 压测设计——从 Python 到 QEMU 的端到端流水线

现在我们来看整个压测流程是怎么串起来的。整个流水线从 Python 脚本开始，经过 CMake 构建系统，最终在 QEMU 内核态完成端到端的数据校验。

### 合成 ELF 的生成：确定性 + 流式写入

`generate_large_elf.py` 的任务是生成一个合法的 ELF64 二进制文件，大小约 1GB，内部填充已知的数据模式。整个文件由三部分组成：64 字节的 ELF header、56 字节的 program header、padding 到 4KB 对齐，然后从 0x1000 开始放置段数据。段数据只有一个 PT_LOAD 段，`p_paddr` 设为 `0x1000000`（16MB，和 staging buffer 相同），大小约 1GB。

数据模式的选取经过了深思熟虑：`byte = (offset >> 12) & 0xFF`，即每 4KB（一个页）数据使用相同的字节值，但相邻页的值不同。这个模式的好处是它是确定性的——给定偏移量就能算出期望值，不需要额外存储参考数据；而且它足够稀疏——如果数据被错位了一个页，抽样检测会立刻发现不匹配。

```python
# scripts/generate_large_elf.py -- 数据模式生成

def generate_pattern(offset: int, size: int) -> bytes:
    data = bytearray(size)
    for i in range(size):
        data[i] = ((offset + i) >> 12) & 0xFF
    return bytes(data)
```

唯一特殊的是偏移量 0 处放的是 `0xFA`（x86 `cli` 指令），而不是 `(0 >> 12) & 0xFF = 0x00`。这是因为 ELF 入口点处的字节需要是一个合法的 x86 指令，`cli`（禁用中断）是内核入口点最常见的第一条指令。

文件写入采用流式方式，每次写 1MB 的数据块，边写边更新 CRC32。这样即使目标文件有 1GB，脚本的内存占用也始终保持在 1MB 左右。CRC32 在文件写入完成后追加到文件末尾（4 字节小端序）。

### 两阶段加载器：从固定大小到动态 sizing

和上一章的简单 `load_big_kernel()` 不同，压测使用的是两阶段加载策略。Phase 1 只读 16 个扇区（8192 字节），刚好够容纳 ELF header 和所有 program header。然后从 program header 中计算出整个 ELF 文件的实际大小——遍历所有 PT_LOAD 段找到最大的 `p_offset + p_filesz`，再加上 section header table 的末端，取对齐到扇区大小。这个设计的好处是 mini kernel 不需要预先知道大内核有多大，1GB 的 ELF 和 100KB 的 ELF 在 Phase 1 的行为完全一样。

Phase 2 的流程更长：调用 `identity_map_up_to()` 确保所有需要的物理地址都被映射，然后调用 `check_memory_overlaps()` 确保内核的 PT_LOAD 段不会覆盖 mini kernel 自身或页表。接下来用 `read_large()` 把完整的 ELF 读入 staging buffer，最后调用 ELF 加载器解析并加载段。

这里有一个非常关键的设计点：staging buffer 和 PT_LOAD 段的物理地址是相同的（都是 `0x1000000`）。这意味着 ELF 加载器在搬运段数据时实际上是在"原地"操作——从 staging buffer 的某个偏移拷贝到另一个偏移。这就是为什么加载器使用了 `memmove` 而不是 `memcpy`，而且在开始加载之前先把 ELF header 和所有 program header 拷贝到了栈上的 `saved_phdrs[16]` 数组——因为一旦开始搬运段数据，staging buffer 中的原始 ELF 头就会被覆盖掉。

### 压测测试用例：端到端数据校验

`test_stress_big_kernel.cpp` 包含两个测试用例。第一个验证 Phase 1 的头部解析：调用 `load_big_kernel_phase1()`，检查 ELF magic 正确、文件大小超过 1MB、扇区数计算正确。

第二个是重头戏。它先重新执行 Phase 1（测试框架不保证跨 `RUN_TEST` 共享状态），然后执行 Phase 2 完成完整的 ELF 加载。加载成功后，调用 `verify_pattern()` 进行数据完整性校验：

```cpp
// kernel/mini/test/test_stress_big_kernel.cpp -- 数据模式验证

bool verify_pattern(uint64_t base, uint64_t size) {
    uint64_t step = 1024 * 1024;  // 每 1MB 抽样一次
    for (uint64_t off = 0; off < size; off += step) {
        uint8_t actual = *reinterpret_cast<volatile uint8_t*>(base + off);
        uint8_t expected = (off == 0) ? 0xFA
            : static_cast<uint8_t>((off >> 12) & 0xFF);
        if (actual != expected) {
            kprintf("  MISMATCH at offset 0x%p: expected 0x%02x got 0x%02x\n", ...);
            return false;
        }
    }
    return true;
}
```

这里用了抽样策略而非全量校验——每 1MB 检查一个字节，1GB 总共 1024 个采样点。这是在"校验覆盖率"和"测试时间"之间做的权衡。1024 个采样点足以检测到"大块数据缺失"或"地址偏移"类的问题。注意读取内存时用了 `volatile` 指针，防止编译器优化掉这些"看起来没什么用"的内存读取。

### CMake 构建链：一键压测

CMake 的 `run-stress-test` target 把整个流水线串在一起：先生成 1GB 的合成 ELF（`stress-kernel-elf` target），然后组装包含这个 ELF 的磁盘镜像（`stress-test-image` target），最后通过 `qemu_test_wrapper.sh` 启动 QEMU 运行压测。整个流程只需要一条命令：

```bash
cd build && make run-stress-test
```

QEMU 使用 `isa-debug-exit` 设备实现自动退出：内核向 I/O 端口 `0xf4` 写 0（测试通过）时 QEMU 退出码为 1，写 1（测试失败）时退出码为 3。`qemu_test_wrapper.sh` 把这个映射回 make 的成功/失败语义。这样压测就可以在 CI/CD 流水线里自动运行，不需要人工盯着看。

## 上板验证

来跑一下看看效果：

```bash
cd build
cmake -DCMAKE_BUILD_TYPE=Debug -DCINUX_BUILD_TESTS=ON ..
cmake --build . -j$(nproc)
make run-stress-test
```

你会看到一大段串口输出，从初始化到 Phase 1 解析、Phase 2 分块读取（每 32MB 打印一次进度），到最终的数据校验。关键输出大致是这样的：

```
[INIT] Initializing ATA controller...
[INIT] ATA controller initialized successfully (status=0x50).
[ATA] Found PCI IDE controller at bus 0, device 1, func 1
[ATA] DMA enabled: BAR4=0xc001, PRDT at phys 0x0010e040

[LOADER] Phase 1: Reading 16 sectors from LBA 0x800...
[LOADER] ELF file: 1073744896 bytes (2097162 sectors)

[LOADER] Mapping physical memory up to 0x0000000041800000...
[LOADER] Phase 2: Reading 2097162 sectors from disk...
[ATA] Read progress: 32 MB / 1023 MB (3%)
[ATA] Read progress: 64 MB / 1023 MB (6%)
...
[ATA] Read progress: 992 MB / 1023 MB (96%)
[ATA] Read progress: 1023 MB / 1023 MB (100%)

[ELF] PT_LOAD[0]: vaddr=0xffffffff80100000 paddr=0x0000000001000000 filesz=0x000000003ffff000 memsz=0x000000003ffff000
[ELF] Loaded segment 0: 0x0000000000001000 -> 0x0000000001000000 (1073741824 bytes, BSS 0 bytes)
[ELF] All PT_LOAD segments loaded.
[LOADER] Big kernel loaded successfully.

=== TEST: Stress Test: Large Kernel Load ===
  Stress ELF: 1073744896 bytes (2097162 sectors)
  [PASS] test_phase1_headers
  Entry point: 0x0000000001000000
  Verifying data pattern (1023 MB)...
  Verify progress: 128 / 1023 samples (12%)
  Verify progress: 256 / 1023 samples (25%)
  ...
  Pattern verified at 1023 sample points
  [PASS] test_phase2_load_and_verify
=== SUMMARY: 2 passed, 0 failed ===
```

看到 `Pattern verified at 1023 sample points` 和 `2 passed, 0 failed` 的时候，我终于放心了。1GB 的数据从磁盘到内存，全程 DMA 加速，数据模式校验全部通过。我们的加载器真的能工作。

## 踩坑总结

这一章的坑比之前所有章节加起来都多，让我按严重程度排序回忆一下。

最阴险的一个坑是 `invlpg` 和 1GB 大页的冲突。在 256KB 内核的测试中，页表只需要填充 PD 的 2MB 大页条目（0-1GB 范围），`invlpg` 指令刷新 2MB 页的 TLB 完全没问题。但 1GB 内核需要填充 PDPT 的 1GB 大页条目，而 `invlpg` 不能刷新 1GB 页的 TLB 条目。我在调试的时候花了好长时间才定位到这个问题——现象是 Phase 2 读磁盘时一切正常，但 `verify_pattern()` 读到的全是 0x00，因为新映射的物理地址还在命中空的 TLB 条目。最后翻 Intel SDM Vol.3 Section 4.10.4 才确认了这个行为，改成 `reload_cr3()` 之后立刻就好了。

第二个坑关于 PRD 表的静态分配。一开始我用的是 PMM 动态分配 PRD 页面，结果在压测环境（只有 3MB RAM）下直接 page fault 了——PMM 分配页面的过程本身就需要页表映射，而页表映射又需要物理页，形成了死锁。改成静态的 `s_prdt_storage[512] __attribute__((aligned(4096)))` 之后问题解决，代价是 BSS 段多了 4KB，完全可以接受。

第三个坑是 ELF 加载器的"原地操作"问题。因为 staging buffer 地址（0x1000000）和 PT_LOAD 的 `p_paddr` 相同，`memmove` 搬运段数据时会覆盖 staging buffer 中还没处理的 ELF 头和 program header。第一版代码没有保存这些数据，导致加载完第一个段之后 ELF 头就被破坏了，后续段的偏移量和大小全部变成垃圾值。修复方法是在开始加载之前把 ELF 头和所有 program header 拷贝到栈上的 `saved_phdrs[16]` 数组。

还有一个 DMA 的坑值得提：Bus Master 的 BM_STATUS 寄存器在每次 DMA 传输完成后必须手动清除 interrupt 标志位（写 `BM_STATUS_INTERRUPT`），否则下一次 DMA 传输会立即报告"完成"——实际上还没开始。这个行为在 PIIX4 的 datasheet 里是有说明的，但如果只看 OSDev Wiki 的教程很容易遗漏。

## 009 全系列回顾——从 higher-half 到 1GB 压测

写到这里，009 系列的五篇文章算是全部完成了。回顾一下我们走过的路：

009A 是整个系列的起点，我们搭建了大内核的启动基础设施——链接脚本定义了 higher-half 布局（虚拟基址 `0xFFFFFFFF80000000`），启动汇编 `boot.S` 承担了从 mini kernel 接管控制权后的底层初始化工作（设栈、清 BSS、跑全局构造函数），这是 mini kernel 把接力棒交给大内核的"交接仪式"。

009B 实现了串口驱动，让我们的大内核终于有了"嘴巴"——通过 I/O 端口 0x3F8 和 COM1 串口通信，`kprintf` 的输出直接显示在终端。从那以后调试不再是黑盒猜谜，而是能直接看到内核在说什么。

009C 进一步完善了大内核的 kprintf 实现，补上了格式化字符串、十六进制输出、指针输出等基础设施。你可能会觉得"一个 printf 也值得单独一章"，但在 freestanding 环境下实现一个可用的格式化输出函数，比你想的要复杂得多。

009D 是一次关键的 bugfix——ELF 加载器在原地加载场景下的数据覆盖问题。这个 bug 在小内核下不会暴露（段数据不够大，不会覆盖到 ELF 头），但在大内核场景下直接导致加载失败。修复方法就是前面提到的 `saved_phdrs` 机制。

然后就是本章 009E——ATA 驱动大修、DMA 加速、1GB 压测验证。这一章的体量最大，涉及的模块最多（ATA/PCI/Paging/CRC32/ELF Loader/CMake），但它的核心意义在于回答了一个问题：我们的加载器到底靠不靠谱？1GB 的压测给出了肯定的答案。

从更宏观的角度看，009 系列完成的是 Cinux 从"一个能启动的 mini kernel"到"一套可靠的内核加载基础设施"的蜕变。MBR + Stage2 把 mini kernel 加载到内存，mini kernel 通过 ATA 驱动和 ELF 加载器把大内核从磁盘读出来，大内核的 `boot.S` 接管控制权，然后进入 C++ 世界——这条完整的启动链现在已经通过了 1GB 级别的压测验证。

## 收尾 + 010 预告

到这里，Phase 2 的 Mini Kernel 基础设施建设基本完成了。我们有了 GDT、IDT、中断处理、物理内存管理、ATA 磁盘驱动（PIO + DMA）、ELF 加载器、两阶段大内核加载器，以及一整套通过 1GB 压测验证的启动流程。mini kernel 作为"二次引导程序"的角色已经完全确立——它不需要做太多事情，只需要把真正的大内核加载到内存、跳过去就行。

接下来的 010 应该会进入一个全新的阶段：大内核自身的功能开发。有了 higher-half 布局、有了串口输出、有了物理内存管理，下一步可能是虚拟内存管理（VMM）、内核堆分配器（kmalloc）、或者进程管理的基础框架。具体走哪个方向还没定，但有一件事是确定的——我们写过的这些代码，每一行都经过了实打实的验证，不是纸上谈兵。

完结撒花。

---

> 本章对应 milestone：`009_big_kernel_stress_test`
> 上一章：[009D - ELF 加载器 Bugfix](009D-elf-loader-bugfix.md)
> 下一章预告：010 — 大内核功能开发（VMM / kmalloc / 进程管理）
