# 009D ELF 加载器踩坑实录 —— 一场由"就地加载"引发的内核崩溃

## 前言

说实话，009A/B/C 这三章走下来，我以为大内核加载的框架已经相当稳固了。ATA 驱动能读磁盘，kprintf 能往串口输出，ELF 解析器能在单元测试里正确解析手工构造的 ELF 头——一切看起来都很美好。然而当我把所有组件串起来，真正用一个编译好的大内核 ELF 文件跑端到端集成测试时，内核直接在加载第一个 PT_LOAD 段之后神秘崩溃。没有异常信息，没有 #PF，没有任何有用的报错——QEMU 直接退出了，留下串口上一行截断的输出，仿佛在嘲笑我之前的自信。

这一章就是这场调试的完整记录。从复现崩溃现象开始，一步步定位到根因——一个经典的 in-place loading 自毁问题——然后修复它。同时，大内核加载器本身也在 milestone 009 中做了大量增强：CRC32 数据完整性校验、内存布局重叠检测、两阶段加载接口重构。我们会在讲完 bug fix 之后把这些增强一并过一遍。

完成本章后，你会看到大内核的 ELF 被完整加载到内存中，所有 PT_LOAD 段正确就位，入口点指令验证通过。35 个测试全部绿灯。

本章的前置知识是 009C（kprintf 格式化输出），因为调试过程严重依赖串口输出。同时你需要熟悉 008 中搭建的 ATA 驱动和 ELF 加载器的基本结构。

---

## 环境说明

我们的开发环境还是那套熟悉的配置：Ubuntu/WSL2 上用 GCC 交叉编译，QEMU 模拟 x86_64 目标机器。内核编译选项为 `-ffreestanding -fno-exceptions -fno-rtti -mcmodel=large -mno-red-zone`，链接时带 `-nostdlib -static`，链接脚本 `kernel/mini/linker.ld` 控制内存布局。测试框架分两层：host 端单元测试（不需要 QEMU，用 `CINUX_HOST_TEST` 宏 mock 硬件依赖）和内核端集成测试（在 QEMU 里跑，串口输出断言结果）。当前 milestone 的 git tag 是 `009_load_large_kernel`。

大内核被链接到 `0xFFFFFFFF80000000` 以上的虚拟地址空间（higher-half kernel），物理基地址为 `0x1000000`（16MB）。这个地址选择直接导致了本章的主角 bug。

---

## 第一步——Bug 复现：当"一切都对了"的时候

运行内核测试，之前的 30 个测试全部通过——kprintf 测试、C++ 特性测试、GDT/IDT 测试、PMM 测试、ATA 驱动测试、ELF 解析器测试，一切正常。然后测试执行到 `Big Kernel Load Tests (009)` 部分，输出是这样的：

```
[RUN] test_big_kernel_load::test_load_elf_success
[ELF] Entry point: 0x0xFFFFFFFF81000000
[ELF] Program headers: 3 at offset 0x0x40
[ELF] PT_LOAD[0]: vaddr=0x0xFFFFFFFF81000000 paddr=0x0x1000000 filesz=0x0x19F0 memsz=0x0x19F0
                                          <-- 输出在此截断
```

"Loaded segment 0" 这条消息从未出现。QEMU 直接退出了，没有任何异常信息——没有 #PF、没有 #GP、没有 general protection fault，什么都没有。因为我们的 QEMU 配置了 `-no-reboot -no-shutdown`，所以 CPU 遇到无法处理的异常时会 triple fault 然后直接关机。

这就是最令人头疼的那种 bug：没有任何错误信息，只有一条被截断的输出，然后整个世界归于寂静。当时我看到这个输出的时候就知道大事不妙——因为这不像是一个"某个检查没过，函数返回了 0"的逻辑错误，而是整个 CPU 直接被带崩了。

---

## 第二步——定位崩溃点：在两条 kprintf 之间

既然输出截断在 `PT_LOAD[0]` 信息和 `Loaded segment` 消息之间，我们就来看 `load_elf()` 函数在这个区间的执行流程。打开 `kernel/mini/elf_loader.cpp`，定位到加载循环，理清楚每一步在干什么：

```
kprintf("PT_LOAD[%u]: vaddr=... paddr=... filesz=... memsz=...", ...)
  ↓ 已输出，正常
边界检查 (staging buffer)
  ↓ if (p_offset + p_filesz > staging_size) return 0;
  ↓ 纯比较操作，不可能崩溃
计算 dest_addr = phdr.p_paddr
计算 src = elf_src + phdr.p_offset
  ↓ 纯算术，不可能崩溃
memcpy(dest, src, filesz)    ← 崩溃嫌疑点
  ↓
memset (BSS 清零)
  ↓ filesz == memsz == 0x19F0，跳过
  ↓
kprintf("Loaded segment ...") ← 从未输出
```

结论非常明确：崩溃发生在 `memcpy` 调用期间或之后的循环迭代中。"Loaded segment" 消息没有出现说明要么 memcpy 本身炸了，要么 memcpy 成功返回后下一次循环迭代出了问题。

但 memcpy 只是一个逐字节拷贝的函数——它怎么可能让 CPU triple fault？除非，它拷贝的目标地址有问题。带着这个思路，我们来看看 PT_LOAD[0] 的关键字段：

- `p_paddr = 0x1000000`（16MB）
- `p_filesz = 0x19F0`（6640 字节）

0x1000000 这个地址看着眼熟吗？没错，它就是 `BIG_KERNEL_LOAD_ADDR`——大内核 ELF 文件从磁盘读到内存后的暂存区（staging buffer）的起始地址。

---

## 第三步——根因分析：staging buffer 与 PT_LOAD 目标重叠

这才是真正精彩的部分。当我们意识到 `p_paddr == BIG_KERNEL_LOAD_ADDR` 的时候，一切都串起来了。让我画一张内存布局图来说明到底发生了什么。

### 加载前的 staging buffer

大内核 ELF 从磁盘读到内存后，从 `0x1000000` 开始布局如下：

```
Staging Buffer @ 0x1000000 (BIG_KERNEL_LOAD_ADDR):
┌──────────────┬──────────────────────────┬──────────────────┐
│  ELF Header  │  Program Headers         │  Segment 0 数据  │
│  (64 bytes)  │  (3 × 56B = 168 bytes)   │  (.text...)      │
│  e_phnum=3   │  Phdr[0]: PT_LOAD        │                  │
│  e_phoff=64  │    p_paddr = 0x1000000   │                  │
│  e_entry=... │  Phdr[1]: PT_LOAD        │                  │
│              │    p_paddr = 0x1002000   │                  │
│              │  Phdr[2]: (non-PT_LOAD)  │                  │
└──────────────┴──────────────────────────┴──────────────────┘
^              ^                            ^
0x1000000      0x1000040                    0x10000E8
```

ELF 头占 64 字节（0x40），Program Header 表从偏移 0x40 开始，3 个 Program Header 各 56 字节共 168 字节，到偏移 0xE8 结束。Segment 0 的文件数据从偏移 0x1000 开始（这是链接器对齐后的结果）。

### 加载 PT_LOAD[0] 时发生了什么

循环到 `i=0` 时，`load_elf()` 要把 Segment 0 的数据从 staging buffer 内部拷贝到 `p_paddr = 0x1000000`：

```
src  = elf_src + 0x1000  (Segment 0 在 staging buffer 内的偏移)
dest = 0x1000000         (p_paddr = staging buffer 的起始地址!)
size = 0x19F0            (6640 字节)
```

`memcpy(0x1000000, 0x1001000, 0x19F0)` —— 把从 0x1001000 开始的 6640 字节拷贝到 0x1000000 开始的位置。问题在于，**目标地址 0x1000000 就是 staging buffer 的起始地址**，而 Segment 0 的数据前 0x1000 字节正好覆盖了 ELF 头和 Program Header 表所在的区域。

```
循环 i=0，执行 memcpy(0x1000000, 0x1001000, 0x19F0) 之后：

Staging Buffer @ 0x1000000:
┌──────────────────────────────┬──────────────────────────────┐
│  Segment 0 数据 (前 0xE8 B)  │  原始 Segment 0 数据         │
│  ↑ 覆盖了 ELF Header!       │                              │
│  ↑ 覆盖了 Program Headers!  │                              │
└──────────────────────────────┴──────────────────────────────┘

此时 staging buffer 开头的 ELF Header 已被 Segment 0 的代码数据覆盖。
ehdr->e_phoff 变成了 Segment 0 的 .text 内容——垃圾值！
```

### 下一次循环迭代：死亡

循环进入 `i=1` 时，代码试图通过 `get_phdr(ehdr, 1)` 来获取第二个 Program Header：

```cpp
const Elf64_Phdr* get_phdr(const Elf64_Ehdr* ehdr, uint16_t index) {
    // ...
    return reinterpret_cast<const Elf64_Phdr*>(
        reinterpret_cast<const uint8_t*>(ehdr) + ehdr->e_phoff  // ← 垃圾值！
        + static_cast<uint64_t>(index) * ehdr->e_phentsize);    // ← 也是垃圾值！
}
```

此时 `ehdr->e_phoff` 不再是原来的 `0x40`——它已经被 Segment 0 的机器码覆盖了，变成了一个不可预测的值。`get_phdr` 用这个垃圾偏移算出来一个野指针，解引用时触发 Page Fault（#PF）。由于我们没有为这个垃圾地址建立页表映射，而且 IDT 里的 #PF handler 本身也可能访问了无效地址，CPU 无法恢复——连续三次异常后 triple fault，QEMU 直接关机。

这就是为什么我们看不到任何错误信息——崩溃发生在 kprintf 之前、在正常的控制流中间、在一条看似无害的指针算术之后。整个崩溃链是：memcpy 覆盖 ELF 头 → 循环迭代读取被覆盖的 e_phoff → 野指针 → #PF → Triple Fault → 关机。

这是一个经典的 **in-place loading 自毁问题**：加载器把数据写到与源缓冲区重叠的目标地址，覆盖了后续迭代所需的元数据。这类 bug 的阴险之处在于，它只在特定的地址布局下才会触发。如果大内核的 `p_paddr` 不等于 staging buffer 地址，或者 Segment 0 的数据没有覆盖到 ELF 头区域，这个 bug 就永远不会出现。

### 为什么之前的单元测试没发现？

008 章的 ELF 加载器单元测试使用手工构造的小 ELF，在栈上分配缓冲区，`p_paddr` 设为一个不与源缓冲区重叠的地址。这种隔离的测试环境天然地回避了 in-place loading 的问题。只有端到端集成测试——用真实的磁盘镜像、真实的链接器输出、真实的物理地址布局——才能暴露这类地址重叠导致的 bug。单元测试和集成测试的互补性在这里体现得淋漓尽致。

---

## 第四步——修复 ELF Loader

定位到根因之后，修复方案其实相当直白。核心思路就是一句话：**在修改 staging buffer 之前，把后续需要的所有信息先保存到栈上**。具体来说有三个修改。

### 修复 1：保存 ELF 头和 Program Headers 到本地变量

打开 `kernel/mini/elf_loader.cpp`，看 `load_elf()` 函数。修复前的代码直接通过 `ehdr` 指针读取 ELF 头字段，一旦 staging buffer 被覆盖，所有后续访问都会读到垃圾数据。修复方式是在进入加载循环之前，把所有需要的字段保存到局部变量中：

```cpp
uint64_t load_elf(void* elf_src, uint64_t staging_size) {
    // Step 1: Validate the ELF header
    if (!parse_elf_header(elf_src)) {
        kprintf("[ELF] ERROR: ELF header validation failed!\n");
        return 0;
    }

    const auto* ehdr = static_cast<const Elf64_Ehdr*>(elf_src);

    // ★ Save header fields before any segment copy.
    // Loading a PT_LOAD segment may write to p_paddr which can overlap
    // the staging buffer (e.g., when the kernel's physical base equals
    // the staging address).  Once that happens the ELF header in the
    // staging buffer is corrupted, so we must capture everything we
    // need up front.
    const uint64_t  saved_entry     = ehdr->e_entry;
    const uint16_t  saved_phnum     = ehdr->e_phnum;
    const uint64_t  saved_phoff     = ehdr->e_phoff;
    const uint16_t  saved_phentsize = ehdr->e_phentsize;

    // ★ Copy all program headers to local storage so they survive
    // staging buffer overwrites during the loading loop.
    if (saved_phnum > 16) {
        kprintf("[ELF] ERROR: too many program headers (%u)\n", saved_phnum);
        return 0;
    }
    Elf64_Phdr saved_phdrs[16];
    for (uint16_t i = 0; i < saved_phnum; i++) {
        const auto* phdr = reinterpret_cast<const Elf64_Phdr*>(
            reinterpret_cast<const uint8_t*>(ehdr) + saved_phoff
            + static_cast<uint64_t>(i) * saved_phentsize);
        saved_phdrs[i] = *phdr;
    }
    // ...
```

这里做了两件事。第一，把 `e_entry`、`e_phnum`、`e_phoff`、`e_phentsize` 这四个后续会用到的字段保存到局部变量中——它们是 `const` 的，存在栈上，staging buffer 怎么被覆盖都不影响。第二，声明一个 `Elf64_Phdr saved_phdrs[16]` 数组，在循环之前就把所有 Program Header 逐个深拷贝过来。后续的加载循环只读 `saved_phdrs[]`，完全不碰 staging buffer 里的头信息。

上限检查 `saved_phnum > 16` 是为了防止恶意或损坏的 ELF 头声称有成千上万个 Program Header 导致栈溢出——16 个 Program Header 对任何内核来说都绰绰有余了。

后续的加载循环也改为使用 `saved_phdrs`：

```cpp
    // Step 5: Iterate through SAVED program headers and load PT_LOAD segments
    for (uint16_t i = 0; i < saved_phnum; i++) {
        const Elf64_Phdr& phdr = saved_phdrs[i];  // ← 从本地数组读取，不是 staging buffer
        if (phdr.p_type != PT_LOAD) continue;
        // ... 正常的拷贝和清零逻辑 ...
    }

    // Return the entry point — using saved_entry, NOT ehdr->e_entry
    constexpr uint64_t HIGHER_HALF_BASE = 0xFFFFFFFF80000000ULL;
    uint64_t entry = saved_entry;
    if (entry >= HIGHER_HALF_BASE) {
        entry = entry - HIGHER_HALF_BASE;
    }
    return entry;
```

注意最后返回入口点时用的是 `saved_entry` 而不是 `ehdr->e_entry`。因为此时 `ehdr` 指向的内存可能已经被 Segment 0 的数据覆盖了——如果用 `ehdr->e_entry`，返回的入口点就是 Segment 0 里的某个随机值，跳过去一定炸。

### 修复 2：memcpy 换成 memmove

这个问题其实更微妙。当 `p_paddr` 等于 staging buffer 起始地址时，`memcpy` 的源和目标虽然有偏移（src = staging + 0x1000，dest = staging），但它们确实重叠了。C 标准明确规定 `memcpy` 要求源和目标不重叠（`__restrict__` 语义），重叠时调用 `memcpy` 是未定义行为。改为 `memmove`：

```cpp
// Before (UB when dest overlaps src):
memcpy(reinterpret_cast<void*>(dest_addr), src, phdr.p_filesz);

// After (correctly handles overlap):
memmove(reinterpret_cast<void*>(dest_addr), src, phdr.p_filesz);
```

你可能会说，我们手写的 `memcpy` 恰好是正向逐字节拷贝——当 `dest < src` 时，从前往后复制不会破坏还没读到的数据，所以"恰好能工作"。但这是一个实现细节，不是语义保证。编译器有权假设 `memcpy` 的源和目标不重叠，据此做更激进的优化（比如 SIMD 批量拷贝、软件预取等），一旦重叠就会出问题。依赖实现细节是未定义行为，今天"恰好能跑"不代表明天换了编译器版本还能跑。用 `memmove` 才是语义上正确的选择。

### 修复 3：测试去重——避免二次调用 load_elf

集成测试文件 `kernel/mini/test/test_big_kernel_load.cpp` 里有一个 `test_entry_address` 测试，它原本的实现是再次调用 `load_elf()` 来获取入口点地址。问题在于，此时 staging buffer 已经被之前的 `test_load_elf_success` 中的段加载操作覆盖了——ELF 魔数不存在了，`parse_elf_header()` 一定会失败。

修复方式是用一个共享变量保存首次加载的结果，后续测试直接使用：

```cpp
// Shared state between tests
static BigKernelLoadState g_state;
static bool g_phase1_ok = false;
static bool g_full_elf_read = false;
static uint64_t g_loaded_entry = 0;  // ← 保存首次加载的入口点

namespace test_big_kernel_load {
    void test_load_elf_success() {
        TEST_ASSERT_TRUE(g_phase1_ok);
        uint64_t entry = load_big_kernel_phase2(g_state, BIG_KERNEL_LBA);
        TEST_ASSERT(entry != 0);
        kprintf("  Entry point: 0x%p\n", reinterpret_cast<void*>(entry));
        g_loaded_entry = entry;  // ← 保存供后续测试使用
    }
}

namespace test_big_kernel_entry {
    void test_entry_address() {
        TEST_ASSERT(g_loaded_entry != 0);         // ← 不再调用 load_elf
        TEST_ASSERT_EQ(g_loaded_entry, BIG_KERNEL_LOAD_ADDR);
    }
}
```

这个修改看似简单，但它暴露了一个重要的设计原则：**ELF 加载是破坏性操作**。一旦 `load_elf()` 执行完毕，staging buffer 里的 ELF 头信息就不再可靠。所有需要在加载后访问的信息（入口点、段地址等），都必须在加载过程中保存下来，不能"回读" staging buffer。

---

## 第五步——大内核加载器增强

Bug fix 搞定之后，我们回头看 milestone 009 对大内核加载器本身做的增强。这些增强和 bug fix 不是一回事——它们是独立的功能改进，但同样重要。

### CRC32 数据完整性校验

从磁盘读取几十 KB 甚至几百 KB 的数据，中间任何一个扇区读错都会导致内核行为异常，而且这种错误通常不会被 ELF 解析器发现——错误的段数据看起来仍然是合法的机器码，只是做了错误的事情。为了在加载之前就能验证数据完整性，我们引入了 CRC32 校验。

CRC32 的实现位于 `kernel/mini/lib/crc32.h`，是一个 header-only 的查找表实现。核心逻辑非常简单：用标准多项式 `0xEDB88320`（reflected 形式）生成 256 项查找表，然后逐字节查表计算。整个表是 `constexpr` 的，编译器会在编译期算好：

```cpp
inline uint32_t crc32(const void* data, size_t len) {
    static constexpr uint32_t table[256] = {
        // 256 个预计算值...
    };
    uint32_t crc = 0xFFFFFFFF;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; i++) {
        crc = table[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}
```

构建端，`scripts/append_crc32.py` 在大内核 ELF 编译完成后自动运行，计算 ELF 文件的 CRC32 值，以小端序追加到文件末尾，然后补齐到 512 字节对齐：

```python
def main():
    path = sys.argv[1]
    data = open(path, "rb").read()
    crc = binascii.crc32(data) & 0xFFFFFFFF
    with open(path, "ab") as f:
        f.write(struct.pack("<I", crc))  # 追加 4 字节 CRC32
    # Pad to 512-byte alignment
```

加载端的测试在 CRC32 校验阶段会把完整的 ELF 读进 staging buffer（Phase 1 只读了头部），然后从 staging buffer 末尾读出构建时追加的 CRC32 值，与重新计算的结果对比。对于大文件还有一个带进度回调的 `crc32_progress()` 版本，每处理 1MB 调用一次回调函数，在串口上输出进度百分比——否则一个几百 MB 的文件算 CRC32 的时候你会以为内核卡死了。

### 内存布局重叠检测

大内核加载器的 Phase 2 增加了内存布局重叠检测功能。加载大内核时，PT_LOAD 段的目标物理地址可能与 mini kernel 自身、页表、或者其他 PT_LOAD 段的区域重叠——这种重叠会导致正在运行的 mini kernel 代码被覆盖，后果就是立即崩溃，而且这种崩溃通常发生在不可预测的位置。

`big_kernel_loader.cpp` 中的 `check_memory_overlaps()` 函数实现了这个检测。它接收一组 `MemoryRegion` 描述符（每个有 start、end、name），两两检查是否有重叠：

```cpp
struct MemoryRegion {
    uint64_t start;
    uint64_t end;      // exclusive
    const char* name;
};

bool check_memory_overlaps(const MemoryRegion* regions, uint32_t count) {
    kprintf("\n=== Memory Layout ===\n");
    for (uint32_t i = 0; i < count; i++) {
        uint64_t size_kb = (regions[i].end - regions[i].start) / 1024;
        kprintf("  %s: 0x%p - 0x%p (%u KB)\n",
                regions[i].name, (void*)regions[i].start,
                (void*)regions[i].end, (uint32_t)size_kb);
    }
    // Check all pairs for overlap
    bool ok = true;
    for (uint32_t i = 0; i < count; i++) {
        for (uint32_t j = i + 1; j < count; j++) {
            if (regions[i].start < regions[j].end &&
                regions[j].start < regions[i].end) {
                kprintf("  OVERLAP: '%s' and '%s' ...\n",
                        regions[i].name, regions[j].name);
                ok = false;
            }
        }
    }
    // ...
}
```

Phase 2 中，在读取完整 ELF 之前，我们会把页表区域（0x1000-0x4000）、mini kernel 区域（0x20000-0x1000000）、以及所有 PT_LOAD 目标区域注册进去，然后调用 `check_memory_overlaps()` 做检测。如果发现重叠就立即终止加载，而不是等段拷贝覆盖了正在运行的代码后再崩溃。注意 staging buffer 本身没有被注册为独立区域——因为 in-place loading（staging buffer 与 PT_LOAD 目标重叠）是预期行为，我们想检测的是那些真正危险的冲突。

### 两阶段加载接口重构

 milestone 008 的 `load_big_kernel()` 是一个一步到位的函数——读固定扇区数、验证、加载。在 009 中，我们把它拆成了两个阶段，用 `BigKernelLoadState` 结构体在两个阶段之间传递信息：

```cpp
struct BigKernelLoadState {
    uint64_t raw_elf_end;       // ELF 数据的实际结束位置
    uint64_t total_elf_size;    // 扇区对齐后的总大小
    uint32_t total_sectors;     // 需要读取的总扇区数
    uint16_t phnum;             // Program Header 数量
    Elf64_Phdr phdrs[16];       // 保存的 Program Headers
};

bool load_big_kernel_phase1(uint64_t disk_lba, BigKernelLoadState& state);
uint64_t load_big_kernel_phase2(const BigKernelLoadState& state, uint64_t disk_lba);
```

Phase 1 只读 16 个扇区（8KB）——刚好够覆盖 ELF 头和所有 Program Header——然后计算出 ELF 文件的总大小。Phase 2 用 Phase 1 算出来的大小扩展页表映射、检查内存重叠、读取完整 ELF、执行段加载。这个拆分的好处是 Phase 1 之后、Phase 2 之前，我们可以插入 CRC32 校验——在完整读取之前先确认数据完整性，避免读了一大堆垃圾数据再发现不对。

同时，`big_kernel_loader.hpp` 中增加了 `MAX_ELF_FILE_SIZE = 0x50000000`（1.25GB）的安全上限，防止损坏的 ELF 头声称文件有几 GB 那么大导致越界读取。

Host 端测试也同步做了增强。`test/unit/test_big_kernel_loader.cpp` 现在覆盖了加载器常量正确性、内存布局约束、磁盘布局算术、ELF 魔数校验逻辑、LBA 范围验证、以及 higher-half 地址转换——这些都是纯逻辑验证，不需要 QEMU 就能在开发机上跑。

---

## 构建与运行

```bash
# 从项目根目录
git checkout 009_load_large_kernel
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCINUX_BUILD_TESTS=ON -S .
cmake --build build -j$(nproc)
cd build

# 运行内核测试（QEMU 内）
make run-kernel-test
```

**期望输出**（Big Kernel Load Tests 部分）：

```
=== Big Kernel Load Tests (009) ===
  [RUN] test_big_kernel_elf_magic::test_elf_magic
  [PASS] test_big_kernel_elf_magic::test_elf_magic
  [RUN] test_big_kernel_crc32::test_crc32_matches
    CRC32: stored=0xdfc9c7aa computed=0xdfc9c7aa (elf_end=28056)
  [PASS] test_big_kernel_crc32::test_crc32_matches
  [RUN] test_big_kernel_load::test_load_elf_success
  [ELF] Entry point: 0x0xFFFFFFFF81000000
  [ELF] Program headers: 3 at offset 0x0x40
  [ELF] PT_LOAD[0]: vaddr=0x0xFFFFFFFF81000000 paddr=0x0x1000000 filesz=0x0x19F0 memsz=0x0x19F0
  [ELF] Loaded segment 0: 0x0x1000 -> 0x0x1000000 (6640 bytes, BSS 0 bytes)
  [ELF] PT_LOAD[1]: vaddr=0x0xFFFFFFFF81002000 paddr=0x0x1002000 filesz=0x0x0 memsz=0x0x5000
  [ELF] Loaded segment 1: 0x0x1000 -> 0x0x1002000 (0 bytes, BSS 20480 bytes)
  [ELF] All PT_LOAD segments loaded.
    Entry point: 0x0x1000000
  [PASS] test_big_kernel_load::test_load_elf_success
  [RUN] test_big_kernel_entry::test_entry_address
  [PASS] test_big_kernel_entry::test_entry_address
  [RUN] test_big_kernel_first_insn::test_first_instruction_is_cli
  [PASS] test_big_kernel_first_insn::test_first_instruction_is_cli

=== Tests: 35 passed, 0 failed ===
=== ALL TESTS PASSED (exit code 0) ===
```

对比修复前截断的输出，现在所有 3 个 PT_LOAD 段都正确处理了——2 个 PT_LOAD 段被加载，1 个非 PT_LOAD 段被跳过。Segment 0（代码段）有 6640 字节的文件数据被拷贝到 0x1000000，Segment 1（BSS 段）没有文件数据但需要 20480 字节的内存清零。入口点 `0xFFFFFFFF81000000` 被正确转换为物理地址 `0x1000000`。最后一条测试验证入口点处的第一个字节是 `0xFA`——这是 `cli` 指令的机器码，大内核入口点的第一条指令就是 `cli`（关中断），这是内核启动时的标准做法。

Host 端测试也可以单独运行验证：

```bash
cd build
make test_host
```

---

## 调试技巧：排查"输出截断"类崩溃的通用思路

这种"输出在某一行截断然后 QEMU 退出"的崩溃模式在内核开发中其实非常常见。这里总结一套通用的排查思路，不局限于这次的 bug。

### 第一步：精确定位截断点

找到最后一条成功输出的 `kprintf` 和第一条未出现的 `kprintf`，确定崩溃发生在两者之间的哪段代码。如果 kprintf 太少，可以临时在可疑区域插入更多的 `kprintf`——但要注意，如果崩溃的原因是某次内存写入破坏了关键数据结构，插入 kprintf 可能改变时序或内存布局，让 bug 消失或转移。这也就是所谓的"printf 调试的海森堡效应"。

### 第二步：检查截断点附近的内存写入操作

截断发生在两条 kprintf 之间，说明这段代码里有某个操作导致了 CPU 异常。最常见的嫌疑对象是：`memcpy`/`memset` 等内存写入（目标地址是野指针或未映射页）、指针解引用（通过损坏的指针访问无效地址）、数组越界写入（覆盖了关键数据结构）。重点检查目标地址是否合理——比如这里的 `p_paddr = 0x1000000`，对照内存布局一看就知道它和 staging buffer 重叠了。

### 第三步：画地址映射图，寻找重叠

在纸上（或文本编辑器里）画出所有关键内存区域的地址范围，标注起始和结束地址。重点关注源缓冲区和目标地址是否有交集。这种可视化能让你一眼看出 "哦，0x1000000 就是 staging buffer 起始地址" 这种关键线索。

### 第四步：用 GDB 辅助定位

如果上述思路不够用，可以用 `make run-kernel-test-debug` 启动 QEMU 调试模式，然后在 GDB 里设断点：

```bash
(gdb) break elf_loader.cpp:203    # 断在 memcpy 调用处
(gdb) continue
(gdb) print dest_addr              # 查看目标地址
(gdb) print src                    # 查看源地址
(gdb) print phdr.p_filesz          # 查看拷贝大小
(gdb) x/8bx (void*)dest_addr       # 查看目标内存内容（拷贝前）
```

GDB 能让你在崩溃发生的精确位置停下，观察所有寄存器和内存值，这比猜测和插入 kprintf 要高效得多。

---

## 经验教训

折腾完这个 bug，有几条值得记住的经验。

**In-place loading 必须先保存元数据。** 当加载器把段数据拷贝到 `p_paddr` 指定的目标地址时，这个目标地址可能与源缓冲区重叠。在进入加载循环之前，必须先把迭代所需的所有头信息（ELF 头字段、Program Header 表）保存到独立的存储中。边读边写同一个缓冲区就是在自毁，这一点不仅适用于 ELF 加载器，也适用于任何需要"原地搬运数据"的场景——比如文件系统中的块搬移、内存压缩算法等等。

**memcpy 和 memmove 不是一回事。** `memcpy` 的语义要求源和目标不重叠，重叠时调用 `memcpy` 是未定义行为。即使你当前的 `memcpy` 实现碰巧能处理重叠（比如逐字节正向拷贝），这只是一个实现细节，不是保证。编译器有权基于 `__restrict__` 语义做更激进的优化，一旦触发就可能静默产生错误结果。当你知道源和目标可能重叠时，永远用 `memmove`。

**单元测试和集成测试互补，缺一不可。** ELF 加载器的单元测试（008 章）使用栈上的小缓冲区，p_paddr 与源不重叠，天然回避了 in-place loading 问题。只有端到端集成测试——用真实的大内核 ELF、真实的物理地址布局——才能暴露这种地址依赖的 bug。单元测试验证的是逻辑正确性，集成测试验证的是系统级交互正确性，两者覆盖的 bug 空间是不同的。

**排查"输出截断"类崩溃要重点关注截断点附近的内存写入。** 当内核输出在某一行中断时，崩溃几乎总是发生在两条 kprintf 之间的内存操作中。精确定位截断点，检查该区间内的所有写入操作的目标地址，画出内存布局图寻找重叠——这套流程效率远高于盲目猜测。

---

## 本章小结

| 组件 | 修改内容 | 说明 |
|------|---------|------|
| `elf_loader.cpp` | 保存 ELF 头和 Program Header 到局部变量 | 解决 in-place loading 导致的头覆盖问题 |
| `elf_loader.cpp` | `memcpy` 替换为 `memmove` | 正确处理源/目标地址重叠 |
| `test_big_kernel_load.cpp` | `g_loaded_entry` 共享变量 | 避免二次调用 `load_elf()` 读取已损坏的 staging buffer |
| `big_kernel_loader.cpp` | Phase 1/2 拆分 + CRC32 校验 + 内存重叠检测 | 大内核加载器的全面增强 |
| `big_kernel_loader.hpp` | `BigKernelLoadState` 结构体 + 新接口 | 两阶段加载的状态传递 |
| `crc32.h` | CRC32 查找表实现（header-only） | 数据完整性校验 |
| `append_crc32.py` | 构建时追加 CRC32 到 ELF 尾部 | 确保构建端和加载端使用相同的校验算法 |
| `test_big_kernel_loader.cpp` | Host 端常量/布局/逻辑测试 | 覆盖加载器的纯逻辑部分 |

这一章的核心教训是一个看似简单的 ELF 加载器，在实际使用中可能踩到多么深的坑。in-place loading 自毁、memcpy UB、测试去重——每一个问题都是"事后看显而易见，事前看毫无征兆"的类型。好消息是，修复之后整个大内核加载管线现在稳如磐石：35 个测试全部通过，从磁盘读取到段加载到入口点验证，端到端没有任何问题。

---

## 下一章预告

下一章（009E）我们会做 ATA 驱动增强——支持大批量扇区读取（`read_large()`），然后用一个 1GB 的合成大内核 ELF 文件做压测。届时会验证两阶段动态加载策略在面对真正巨大的文件时是否仍然可靠，以及 CRC32 校验在大数据量下的表现。同时我们也会关注页表动态扩展的性能开销——毕竟 1GB 的数据意味着需要映射几百个 2MB 大页。
