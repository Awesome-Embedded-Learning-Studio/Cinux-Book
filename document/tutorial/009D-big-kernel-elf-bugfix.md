# ELF 加载器踩坑实录：一个 in-place loading 的经典陷阱，或者"我是怎么被自己的 memcpy 杀死的"

> 作者：
> 标签：x86-64, ELF64, in-place loading, memcpy vs memmove, OS 开发调试, staging buffer, QEMU, 裸机开发

---

## 前言

说实话，这个 bug 差点让我以为整个 loader 要重写。

到 milestone 008 结束的时候，我们已经有了一套看起来非常完整的加载管线：ATA PIO 磁盘驱动能读扇区，ELF 解析器能验证文件头和加载 PT_LOAD 段，big kernel loader 把两者串在一起。单元测试全部绿灯，九个测试用例覆盖了从 magic 校验到段加载的各种边界情况，一切都很美好。然后我信心满满地写了一个集成测试——让 mini kernel 真正从磁盘读取大内核 ELF 并加载它——然后 QEMU 黑屏了。没有异常信息，没有 panic 输出，串口日志在打印完 PT_LOAD[0] 的地址信息后就戛然而止，紧接着 QEMU 直接退出。

如果你也做过内核开发，你一定知道那种感觉：日志在某个 `kprintf` 之后凭空消失，没有 `#PF`、没有 `#GP`、没有任何异常编号——只有死一般的沉默。这说明 CPU 触发了 triple fault（通常是 Page Fault 之后中断处理不可用导致的 double fault，再之后就是 triple fault，处理器直接复位），而我们的 IDT 还没有来得及把异常信息打出来就挂了。

排查这个 bug 的过程堪称教科书级别的"in-place loading 陷阱"——ELF 加载器把段数据从 staging buffer 搬运到目标物理地址时，目标地址恰好就是 staging buffer 自身。于是搬运第一个段的同时，ELF 头被段数据覆盖了。后续循环通过已经变成垃圾的 ELF 头去索引下一个 Program Header，算出一个野指针，解引用之后直接炸飞。这不是"代码写错了"，而是"架构选择带来了一个隐含的前提条件，而这个前提条件在真实场景下被打破了"——单元测试用的小 ELF 永远不会触发这个问题，因为手工构造的 `p_paddr` 和源缓冲区根本不重叠。

这篇文章就是完整的排查过程，从现象到定位，从根因到修复，再到增强验证。我会尽量还原当时的思考过程——包括那些走弯路的部分。

## 环境说明

实验环境和之前一样：x86_64 平台，GNU AS + GCC/G++ + CMake 构建，QEMU 模拟运行，内核 freestanding C++23，无标准库。几个关键参数需要提前交代清楚：

`BIG_KERNEL_LOAD_ADDR` 定义在 `big_kernel_loader.hpp` 里，值为 `0x1000000`（16MB）。这是大内核 ELF 数据从磁盘读到内存后暂存的位置（staging buffer）。大内核的 ELF 是 higher-half 编译的，链接脚本中物理地址基址从 16MB 开始——也就是说 `PT_LOAD[0].p_paddr` 也是 `0x1000000`。这两个地址相等不是巧合，而是设计使然：staging buffer 选在 16MB，大内核的物理加载地址也在 16MB，在正常情况下这意味着段数据从 staging buffer 搬到它"应去的地方"时，如果"应去的地方"就是 staging buffer 自己呢？

QEMU 配置了 `-no-reboot -no-shutdown`，意味着 triple fault 之后 QEMU 不会重启而是直接退出——这在调试"无故重启"类 bug 时非常有用，否则你会看到 QEMU 不停地重启循环，根本来不及看输出。

## Bug 复现——输出截断的完整过程

事情发生在 `run_big_kernel_load_tests()` 这个集成测试函数里。测试的结构很简单：Phase 1 从磁盘读取 ELF 头部、验证 magic、解析 Program Header、计算总文件大小；Phase 2 扩展页表映射、检查内存重叠、读取完整 ELF、调用 `load_elf` 加载所有 PT_LOAD 段。

Phase 1 一切正常，输出是这样的：

```
=== Big Kernel Load Tests (009) ===
[LOADER] Phase 1: Reading 16 sectors from LBA 0x350...
[LOADER] ELF file: 28032 bytes (55 sectors)
[RUN] test_big_kernel_elf_magic::test_elf_magic
[PASS] test_big_kernel_elf_magic::test_elf_magic
```

ELF magic 校验通过了，说明磁盘读取和头验证都是好的。接下来是 CRC32 校验：

```
[RUN] test_big_kernel_crc32::test_crc32_matches
  CRC32: stored=0xdfc9c7aa computed=0xdfc9c7aa (elf_end=28056)
[PASS] test_big_kernel_crc32::test_crc32_matches
```

CRC32 也匹配了，说明从磁盘到内存的数据传输是完全无损的。到这里一切正常。然后执行 Phase 2，调用 `load_elf()` 开始加载段：

```
[RUN] test_big_kernel_load::test_load_elf_success
[ELF] Entry point: 0x0xFFFFFFFF81000000
[ELF] Program headers: 3 at offset 0x0x40
[ELF] PT_LOAD[0]: vaddr=0x0xFFFFFFFF81000000 paddr=0x0x1000000 filesz=0x0x19F0 memsz=0x0x19F0
```

输出在这里戛然而止。按照代码逻辑，`kprintf("PT_LOAD[0]: ...")` 之后应该立刻执行边界检查、`memcpy`/`memmove` 段数据、`memset` 清零 BSS，然后打印 `"Loaded segment 0: ..."`。但这条 "Loaded segment" 消息从未出现，QEMU 直接退出了。更诡异的是，没有任何异常编号输出——没有 `[EXCEPTION] Page Fault`，没有 `#GP`，什么都没有，只有死一般的沉默。

## 定位——从现象到崩溃点

输出截断在 `PT_LOAD[0]` 信息和 `Loaded segment 0` 之间，这意味着崩溃发生在那个区间的代码中。我们来仔细看一下 `load_elf()` 中那个区间的执行流程：

```cpp
// 这条已经输出了
kprintf("[ELF] PT_LOAD[%u]: vaddr=0x%p paddr=0x%p filesz=0x%p memsz=0x%p\n", ...);

// 接下来是这几步：
// 1. 边界检查 —— staging_size 校验，纯比较操作，不可能崩溃
// 2. memcpy(dest, src, filesz) —— 把段数据从 staging buffer 搬到 p_paddr
// 3. memset(bss, 0, ...) —— BSS 清零（本例中 filesz == memsz，跳过）
// 4. kprintf("Loaded segment ...") —— ← 从未输出

// 这条从未输出
kprintf("[ELF] Loaded segment %u: ...\n", ...);
```

边界检查是纯比较操作，不可能崩溃。`filesz == memsz` 所以 BSS 清零被跳过了。那么嫌疑就集中在一个地方：`memcpy`。但等等——如果 `memcpy` 崩溃了，那至少应该触发一个 `#PF` 吧？我们的 IDT 已经配好了，Page Fault handler 应该会打印异常信息才对。为什么什么都没有？

这里有一个关键的推断：如果 CPU 在 `memcpy` 之后、下一次 `kprintf` 之前的地方崩溃了，而且没有触发任何可捕获的异常，那说明崩溃发生在循环回来到 `i=1` 的迭代中——具体来说，是发生在获取下一个 Program Header 的时候。`get_phdr(ehdr, 1)` 需要读取 `ehdr->e_phoff`，如果这个值已经被垃圾覆盖，算出来的就是一个野指针，解引用野指针会触发 `#PF`。但如果 `#PF` handler 自身也无法正常工作（比如栈被破坏了，或者 IDT 的中断门指向了无效地址），那 `#PF` 就会升级成 double fault，再升级成 triple fault，QEMU 直接关机。

现在问题变成了：为什么 `ehdr->e_phoff` 会变成垃圾值？ELF 头在 `parse_elf_header` 的时候明明是好的。

## 根因——staging buffer 与 p_paddr 重叠的内存布局

答案藏在地址里。我们来对照一下几个关键地址：

```
BIG_KERNEL_LOAD_ADDR（staging buffer）= 0x1000000（16MB）
PT_LOAD[0].p_paddr（段目标物理地址）= 0x1000000（16MB）
```

同一个地址。staging buffer 的起始地址和大内核第一个 PT_LOAD 段的目标物理地址完全相同。这不是 bug——这是设计使然。大内核的链接脚本把物理基址设在 16MB，staging buffer 也选在 16MB，两者天然重叠。

现在我们来看看 `memcpy` 到底做了什么：

```
memcpy(
    dest = p_paddr = 0x1000000,           // 目标 = staging buffer 起始
    src  = staging + p_offset = 0x1000000 + 0x78,  // 源 = staging buffer 内部偏移
    size = p_filesz = 0x19F0              // 拷贝 6640 字节
)
```

`p_offset = 0x78` 是段数据在 ELF 文件中的起始偏移——前面 64 字节是 ELF 头，紧接着是 3 个 Program Header 每个 56 字节，加起来正好 232 字节（0xE8），段数据从 0x78 开始（实际上对齐后从那里开始）。这意味着 `memcpy` 把 staging buffer 中偏移 0x78 处的数据，拷贝到 staging buffer 的起始地址——也就是覆盖了 ELF 头和前面的 Program Header。

来画一张内存布局图，这个过程就一目了然了：

```
memcpy 之前的 staging buffer (0x1000000):

0x1000000  ┌──────────────────┐
           │ ELF Header       │  e_ident, e_entry=0xFFF..., e_phoff=0x40, e_phnum=3
           │ (64 bytes)       │
0x1000040  ├──────────────────┤
           │ Phdr[0]          │  PT_LOAD: paddr=0x1000000, offset=0x78, filesz=0x19F0
           │ Phdr[1]          │  PT_LOAD: paddr=0x1002000, memsz=0x5000 (BSS)
           │ Phdr[2]          │  (non-PT_LOAD)
0x10000E8  ├──────────────────┤
           │ Segment 0 data   │  .text 代码段，第一条指令是 cli (0xFA)
           │ ...              │
           │ (0x19F0 bytes)   │
0x1001AD8  └──────────────────┘

           ↓ memcpy(0x1000000, 0x1000078, 0x19F0) ↓

memcpy 之后的 staging buffer (0x1000000):

0x1000000  ┌──────────────────┐
           │ Segment 0 data   │  ← 前面的 ELF 头和 Phdr 被段数据覆盖了！
           │ (前 0x78 字节    │     ehdr->e_phoff 现在是段数据中的某个随机值
           │  覆盖了 ELF 头)  │     ehdr->e_phnum 也是垃圾
           │ ...              │
0x1001AD8  └──────────────────┘

           ↓ 循环 i=1, get_phdr(ehdr, 1) ↓
           → 读 ehdr->e_phoff → 垃圾值（比如 0xFA = cli 指令的字节）
           → 计算野指针 → 解引用 → #PF → Triple Fault → QEMU 退出 💥
```

整个过程可以拆解成三步：第一步，`memcpy` 把段数据写到 staging buffer 的起始位置，覆盖了 ELF 头和 Program Header。由于 `dest < src`（0x1000000 < 0x1000078），正向逐字节拷贝不会破坏尚未读取的源数据，所以 `memcpy` 本身"成功完成"了。第二步，循环推进到 `i=1`，`get_phdr(ehdr, 1)` 尝试从已经被覆盖的 ELF 头中读取 `e_phoff`，读到的已经不是原来的 `0x40` 而是段数据中的某个字节值。第三步，用这个垃圾值算出一个 Program Header 的地址，解引用之后触发 Page Fault。由于某种原因（很可能是异常处理过程中栈或 IDT 状态异常），Page Fault 升级为 triple fault，QEMU 直接退出——这就是为什么我们看不到任何异常信息。

你可能会问：为什么之前的单元测试没发现这个问题？答案很简单——ELF Loader 的单元测试使用手工构造的小 ELF 在栈上运行，`p_paddr` 是一个随便填的值，和源缓冲区的地址完全不重叠。in-place loading 只有在用真实的大内核 ELF 做端到端测试时才会发生，因为只有真实内核的 `p_paddr` 才会恰好等于 staging buffer 的地址。这是一个经典的"单元测试和集成测试互补"案例——单元测试验证逻辑正确性，集成测试暴露环境相关的 bug。

## 修复——三步走方案

找到了根因之后，修复方案其实很直观。核心思路是：在 staging buffer 被覆盖之前，把所有后续需要的元数据"抢救"到栈上的本地变量里。

### 第一步：保存 ELF 头字段和所有 Program Header

修改位置在 `kernel/mini/elf_loader.cpp` 的 `load_elf()` 函数。修复前的代码在验证 ELF 头之后，直接拿着 `ehdr` 指针在循环里通过 `get_phdr(ehdr, i)` 访问 Program Header。修复后的代码在进入循环之前，先把所有需要的信息深拷贝到本地：

```cpp
// Save header fields before any segment copy.
// Loading a PT_LOAD segment may write to p_paddr which can overlap
// the staging buffer (e.g., when the kernel's physical base equals
// the staging address).  Once that happens the ELF header in the
// staging buffer is corrupted, so we must capture everything we
// need up front.
const uint64_t  saved_entry     = ehdr->e_entry;
const uint16_t  saved_phnum     = ehdr->e_phnum;
const uint64_t  saved_phoff     = ehdr->e_phoff;
const uint16_t  saved_phentsize = ehdr->e_phentsize;

// Copy all program headers to local storage so they survive staging
// buffer overwrites during the loading loop.
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
```

四个 `saved_*` 变量保存了 ELF 头中后续需要的关键字段——入口地址、Program Header 数量、偏移、单个大小。然后最关键的一步是 `saved_phdrs[16]`——把所有 Program Header 深拷贝到栈上的本地数组。结构体赋值 `saved_phdrs[i] = *phdr` 会触发一次完整的 56 字节拷贝，把每个 Program Header 的所有字段（`p_type`、`p_flags`、`p_offset`、`p_vaddr`、`p_paddr`、`p_filesz`、`p_memsz`、`p_align`）都复制到栈上。有了这些备份之后，加载循环就完全不依赖 staging buffer 的头部了。

我们限制了最多 16 个 Program Header，超过就报错。对于内核 ELF 来说这远远够用——我们的大内核只有 3 个。这个限制是一个安全阈值，防止损坏的 ELF 头声称有几千个 Program Header 导致栈溢出。

### 第二步：加载循环使用本地副本

修复后的加载循环不再通过 `get_phdr(ehdr, i)` 访问 staging buffer，而是直接用 `saved_phdrs[i]`：

```cpp
// Step 5: Iterate through saved program headers and load PT_LOAD segments
for (uint16_t i = 0; i < saved_phnum; i++) {
    const Elf64_Phdr& phdr = saved_phdrs[i];  // ← 从本地副本读，不碰 staging buffer

    if (phdr.p_type != PT_LOAD) {
        continue;
    }

    // ... 边界检查、段拷贝、BSS 清零 ...
}

// Step 7: Return the entry point address (physical)
constexpr uint64_t HIGHER_HALF_BASE = 0xFFFFFFFF80000000ULL;
uint64_t entry = saved_entry;  // ← 从本地变量读，不回读 ELF 头
if (entry >= HIGHER_HALF_BASE) {
    entry = entry - HIGHER_HALF_BASE;
}
return entry;
```

注意循环变量从 `ehdr->e_phnum` 变成了 `saved_phnum`，Program Header 的访问从 `get_phdr(ehdr, i)` 变成了 `saved_phdrs[i]`，最后返回入口地址时用的是 `saved_entry` 而不是 `ehdr->e_entry`。整个逻辑没有变，只是数据来源从"staging buffer 中可能被覆盖的 ELF 头"变成了"栈上安全的本地副本"。

### 第三步：memcpy 替换为 memmove

当 `p_paddr` 和 staging buffer 的源地址重叠时，`memcpy` 的行为在 C 标准里是未定义的——函数签名的 `__restrict__` 语义要求调用者保证源和目标不重叠。虽然我们的手写 `memcpy` 是正向逐字节拷贝，在 `dest < src` 的场景下碰巧不会破坏未读数据，但依赖"碰巧正确"的实现细节是内核开发中最危险的事情之一。

来看一下我们手写的 `memcpy` 和 `memmove` 的实现：

```cpp
void* memcpy(void* __restrict__ dest, const void* __restrict__ src, size_t count) {
    uint8_t* d = static_cast<uint8_t*>(dest);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < count; i++) {
        d[i] = s[i];  // 正向逐字节拷贝
    }
    return dest;
}

void* memmove(void* dest, const void* src, size_t count) {
    uint8_t* d = static_cast<uint8_t*>(dest);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    if (d < s) {
        for (size_t i = 0; i < count; i++) {
            d[i] = s[i];  // 正向拷贝
        }
    } else if (d > s) {
        for (size_t i = count; i > 0; i--) {
            d[i - 1] = s[i - 1];  // 反向拷贝，避免覆盖未读数据
        }
    }
    return dest;
}
```

`memmove` 多了一次地址比较：当 `dest < src` 时正向拷贝（和 `memcpy` 一样），当 `dest > src` 时反向拷贝（从后往前写），确保在任何重叠模式下都不会破坏尚未读取的源数据。在我们的场景中 `dest = 0x1000000`，`src = 0x1000078`，`dest < src`，所以 `memmove` 走的是正向拷贝路径——行为和 `memcpy` 完全相同。但使用 `memmove` 是语义正确性的保证：将来如果某天我们换用一个优化过的 `memcpy`（比如用 SIMD 指令做批量拷贝，可能先写高地址再写低地址），行为就可能突然改变。`memmove` 的额外开销只有一次指针比较，但换来的是绝对的正确性保证。

修复就是一行代码的事：

```cpp
// 修复前：memcpy(reinterpret_cast<void*>(dest_addr), src, phdr.p_filesz);
// 修复后：
memmove(reinterpret_cast<void*>(dest_addr), src, phdr.p_filesz);
```

## 加载器增强——CRC32 校验与内存验证

Bug 修复的同时，我们还给加载管线加了两个重要的验证机制。

### CRC32 完整性校验

CRC32 校验是在 Phase 1（读头部）和 Phase 2（加载段）之间执行的。构建脚本 `append_crc32.py` 在大内核 ELF 文件末尾追加一个 4 字节的 CRC32 校验和。测试代码在 Phase 1 计算出 ELF 文件的实际大小之后，读取完整的 ELF 到 staging buffer，然后用 `crc32_progress()` 计算 CRC32 并与磁盘上存储的值比对：

```cpp
// CRC32 is stored as 4 bytes immediately after the actual ELF data
const auto* stored_crc_ptr =
    reinterpret_cast<const uint32_t*>(staging + g_state.raw_elf_end);
uint32_t stored_crc = *stored_crc_ptr;

// Compute CRC32 over the ELF data (with progress for large files)
auto crc_progress = [](size_t done, size_t total) {
    uint32_t pct = static_cast<uint32_t>((done * 100) / total);
    kprintf("  CRC progress: %u / %u bytes (%u%%)\n",
            static_cast<uint32_t>(done),
            static_cast<uint32_t>(total), pct);
};
uint32_t computed_crc = crc32_progress(staging, g_state.raw_elf_end,
    crc_progress, 1024 * 1024);  // report every 1MB

kprintf("  CRC32: stored=0x%x computed=0x%x (elf_end=%u)\n",
        stored_crc, computed_crc, static_cast<uint32_t>(g_state.raw_elf_end));

TEST_ASSERT_EQ(stored_crc, computed_crc);
```

CRC32 使用标准的多项式 `0xEDB88320`（reflected），用查表法实现，每个字节只需要一次查表和两次异或。对于大文件（比如后续的 1GB 压力测试），`crc32_progress` 变体每处理 1MB 就回调一次进度函数，这样在串口输出上能看到实时进度。CRC32 校验和 bug 修复没有直接关系，但它显著提升了端到端的可信度——如果 CRC32 匹配，说明从构建脚本写入磁盘、到 ATA 驱动读取扇区、到 staging buffer 中的数据，每一步都是无损的。

### 内存重叠检查

`big_kernel_loader` 的 Phase 2 在加载段之前会做一次全面的内存重叠检查，注册所有关键内存区域（页表、mini kernel、每个 PT_LOAD 的目标地址范围），然后两两比较是否有重叠：

```cpp
MemoryRegion regions[MAX_MEMORY_REGIONS];
uint32_t rcount = 0;

// Page tables (fixed by bootloader)
regions[rcount++] = {0x1000, 0x4000, "Page Tables"};

// Mini kernel (approximate)
regions[rcount++] = {MINI_KERNEL_LOAD_ADDR, BIG_KERNEL_LOAD_ADDR, "Mini Kernel"};

// PT_LOAD target regions
for (uint16_t i = 0; i < state.phnum; i++) {
    if (state.phdrs[i].p_type == PT_LOAD && state.phdrs[i].p_memsz > 0) {
        regions[rcount].start = state.phdrs[i].p_paddr;
        regions[rcount].end = state.phdrs[i].p_paddr + state.phdrs[i].p_memsz;
        regions[rcount].name = "PT_LOAD target";
        rcount++;
    }
}

if (!check_memory_overlaps(regions, rcount)) {
    kprintf("[LOADER] FATAL: Memory overlap detected, aborting load!\n");
    return 0;
}
```

这里有一个微妙的设计决策：staging buffer 没有被注册为单独的内存区域。这是因为 staging buffer 和 PT_LOAD 目标天然重叠（这就是我们要用 in-place loading 的原因），如果我们把它也加进去，每次都会报告重叠错误。我们真正想要检测的危险重叠是：PT_LOAD 目标覆盖了正在运行的 mini kernel 代码（那会直接崩溃），或者多个 PT_LOAD 目标之间互相覆盖。staging buffer 与 PT_LOAD 的重叠是预期行为，不是错误。

### 两阶段加载：从固定读取到动态 sizing

Bug 修复前后，`big_kernel_loader` 经历了一次重要的架构升级。之前是简单粗暴地一次性读取 512 个扇区（256KB）到 staging buffer——这个数字是拍脑袋定的，"应该够用了"。但这种方式有明显的缺陷：如果内核 ELF 超过 256KB 就读不全，如果只有几 KB 就浪费了大量读取时间。

修复后的两阶段加载策略是这样的：Phase 1 只读 16 个扇区（8KB），刚好覆盖 ELF 头和所有 Program Header。从 Program Header 中可以计算出 ELF 文件的实际大小（遍历所有 PT_LOAD 段的 `p_offset + p_filesz`，取最大值），然后 Phase 2 按需读取完整数据。这种方式支持从几 KB 到 1.25GB 的 ELF 文件——上限由 `MAX_ELF_FILE_SIZE = 0x50000000` 限制，防止损坏的 ELF 头声称文件有几百 GB 导致内核读取越界。

`BigKernelLoadState` 结构体是两阶段之间传递状态的桥梁，包含了 Phase 1 计算出的所有信息：`raw_elf_end`（实际数据结束位置）、`total_elf_size`（扇区对齐后的总大小）、`total_sectors`（需要读取的扇区数）、`phnum`（Program Header 数量）和 `phdrs[]`（深拷贝的 Program Header 数组）。你可能会注意到 `big_kernel_loader` 和 `elf_loader` 各自保存了一份 Program Header——这是正确的分层设计。`big_kernel_loader` 需要在 Phase 1 和 Phase 2 之间传递状态（比如做 CRC32 校验、计算页表映射范围），`elf_loader` 需要在加载循环中保护自己不受 in-place 覆盖影响。两者保存副本的原因不同，各自独立完成是最清晰的做法。

## 上板验证——修复后的完整测试输出

修复完成之后，重新构建并运行测试。全部 35 个测试通过，串口输出如下：

```
=== Big Kernel Load Tests (009) ===
[LOADER] Phase 1: Reading 16 sectors from LBA 0x350...
[LOADER] ELF file: 28032 bytes (55 sectors)
[RUN] test_big_kernel_elf_magic::test_elf_magic
[PASS] test_big_kernel_elf_magic::test_elf_magic
[RUN] test_big_kernel_crc32::test_crc32_matches
  CRC: Reading full ELF (55 sectors) into staging buffer...
  CRC: Full ELF read complete.
  CRC: Computing CRC32 over 28056 bytes...
  CRC32: stored=0xdfc9c7aa computed=0xdfc9c7aa (elf_end=28056)
[PASS] test_big_kernel_crc32::test_crc32_matches
[RUN] test_big_kernel_load::test_load_elf_success

=== Memory Layout ===
  Page Tables: 0x00001000 - 0x00004000 (4 KB)
  Mini Kernel: 0x00020000 - 0x01000000 (16256 KB)
  PT_LOAD target: 0x01000000 - 0x010019F0 (6 KB)
  PT_LOAD target: 0x01002000 - 0x01007000 (20 KB)
  [OK] No overlaps detected.
=====================

[LOADER] Staging buffer: 0x01000000 - 0x01006E00 (27 KB)
[LOADER] Phase 2: Reading 55 sectors from disk...
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

对比修复前的截断输出，现在我们能看到完整的加载过程了。三个关键变化值得注意：

第一，`PT_LOAD[0]` 之后终于出现了 `"Loaded segment 0"`——说明段拷贝成功完成了，没有再崩溃。这是因为加载循环使用的是 `saved_phdrs[]` 本地副本，即使 staging buffer 的 ELF 头被段数据覆盖了，循环也不受影响。

第二，两个 PT_LOAD 段都成功加载了。PT_LOAD[0] 是代码段（6640 字节的 `.text`），目标地址 `0x1000000`；PT_LOAD[1] 是 BSS 段（20480 字节零填充），目标地址 `0x1002000`。第三个 Program Header 不是 PT_LOAD 类型，被跳过了。

第三，内存布局检查显示 "No overlaps detected"——除了 staging buffer 和 PT_LOAD[0] 的预期重叠之外，没有其他危险的重叠。Page Tables（4KB）、Mini Kernel（约 16MB）、两个 PT_LOAD 目标（共 26KB）之间互不冲突。

最后，首条指令验证测试确认入口地址 `0x1000000` 处的第一个字节是 `0xFA`——这正是大内核启动汇编中 `cli` 指令的机器码，说明加载出来的代码是正确的。

## 经验教训

排查这个 bug 的过程中有几个教训值得记录下来，以后再遇到类似的"输出截断"类崩溃时可以快速定位。

### In-place loading 必须先保存元数据

这是一个通用的原则，不限于 ELF 加载器。任何"边读边写同一个缓冲区"的场景——解压缩、DMA 双缓冲、ELF 段加载——都需要在覆盖之前把迭代所需的元数据保存到独立的存储中。你可以把它理解为：我们是在一个黑板上写解题过程，但解题过程本身需要参考黑板上已有的公式——如果你先把公式擦了再写答案，那就全乱了。必须先把公式抄到草稿纸上，然后才能放心地擦黑板。

### memcpy 的重叠语义不是"建议"而是"契约"

C 标准规定 `memcpy` 的源和目标不重叠（`__restrict__` 语义），这不是一个"最好遵守"的建议，而是一个硬性的函数契约。违反契约就是未定义行为——即使当前的实现碰巧能工作，将来编译器优化、库替换、平台迁移都可能导致行为突然改变。在内核开发中，"碰巧正确"是最危险的，因为它会在你最不期望的时候突然不正确。`memmove` 的额外开销微乎其微（一次指针比较），但换来的是语义上的绝对安全。

### "输出截断"类崩溃的排查思路

当内核测试输出在某一行中断时，排查的步骤是这样的：首先确认截断发生在两条 `kprintf` 之间的哪段代码中——输出是同步的，打印出来的最后一行之前的代码都是安全的，问题出在那之后的代码中。然后检查那段代码中的内存写入操作——是不是写了不该写的地址。接着思考写入目标地址是否可能破坏正在使用的数据结构——在我们的案例中，写入目标覆盖了 ELF 头，而后续循环还在读 ELF 头。最后画出地址映射图，寻找重叠区域——一旦画出来，问题就一目了然了。

### 单元测试和集成测试互补

这个 bug 是一个教科书级别的案例：单元测试全部绿灯，集成测试立刻暴露问题。原因在于单元测试使用手工构造的测试数据，而手工数据不可避免地带有构造者的偏见——我们构造小 ELF 时不会故意让 `p_paddr` 指向源缓冲区，因为"那不合理"。但在真实场景中，`p_paddr` 等于 staging buffer 地址是再自然不过的事。某些 bug 只有用真实数据流才能暴露出来。

## 收尾

到这里，ELF 加载器的 in-place loading 陷阱就被彻底解决了。修复涉及三个互相配合的部分：`elf_loader.cpp` 在加载前保存 ELF 头和 Program Header 到本地变量、`memcpy` 替换为 `memmove`、测试代码用共享变量避免在 staging buffer 被破坏后重复调用 `load_elf`。同时，加载管线增加了 CRC32 完整性校验和内存重叠检查，升级为动态 sizing 的两阶段加载策略。

从架构的角度看，这次 bug 修复不是"补了一个洞"，而是让加载器从一个"只在特定条件下正确工作"的实现，进化为一个"在任何地址布局下都安全"的通用实现。无论未来大内核的链接脚本怎么改、物理地址怎么变，`saved_phdrs` + `memmove` 的组合都能保证 in-place loading 的安全性。

下一步，我们要给这套加载管线来一次真正的压力测试——生成一个 1GB 的合成 ELF，让加载器处理超大文件的动态 sizing、CRC32 流式校验和页表扩展。那将是 Cinux 加载能力的终极考验。

---

> 本章对应 milestone：`009_load_large_kernel`
> 上一章：[008 - 磁盘驱动与 ELF 加载器](008-mini-kernel-disk-and-loader.md)
> 下一章预告：009E — 1GB 大内核压力测试
