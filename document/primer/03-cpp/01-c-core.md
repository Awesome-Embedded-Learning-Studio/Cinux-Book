---
title: 01 · C 核心:指针、位、布局与 MMIO
---

# 01 · C 核心:指针、位、布局与 MMIO(内核视角)

> 这一章把正文 004 之后内核代码里反复出现的几样东西提前讲透:为什么到处是 `uint64_t` 而不是 `int`、为什么页表项要用位域 + `__attribute__((packed))`、为什么 AHCI 寄存器全是 `volatile` 但 `volatile` 又挡不住 CPU 乱序、以及怎么用 `static_assert`/`offsetof` 在编译期把布局钉死。读完你应该能无障碍地读懂正文从 004(C++ 内核起点)开始的结构体定义和内联汇编。

正文里 Cinux 用的是 C++(`.hpp`/`.cpp`),但这一章讲的全是 **C 与 C++ 共用的那部分内核子集**——freestanding、没有 STL、没有异常、没有 RTTI、没有智能指针,只有裸指针、整数和位。我们刻意只讲这一窄条,因为内核里用到的 C++ 特性本来就不多,先把这块地基夯实,正文里那些 `struct [[gnu::packed]]`、`volatile uint32_t`、`__asm__ volatile(...:::"memory")` 就不再突兀。

## 一、指针、数组退化、固定宽度整数

### 1. `uint8_t`/`uint32_t`/`uint64_t`:宽度必须由你说了算

内核里几乎所有整数都用 `<stdint.h>` 的**固定宽度整数**。这是 C99 起的标准头,在 C++ 里同样可用(Cinux 的 `.hpp` 里 `#include <stdint.h>`)。看一眼 ELF 头是怎么定义的:

```cpp
// kernel/proc/elf_types.hpp:60-75
struct Elf64_Ehdr {
    uint8_t  e_ident[16];  // ELF identification bytes
    uint16_t e_type;       // Object file type (ET_EXEC = 2)
    uint32_t e_version;
    uint64_t e_entry;      // Virtual entry point address
    ...
    uint16_t e_ehsize;     // ELF header size (64 bytes)
    uint16_t e_phentsize;  // Program header entry size (56 bytes)
    ...
} __attribute__((packed));
```

为什么不用 `int`/`long`?因为 **ELF 文件格式是写死的**:第 0–15 字节就是 `e_ident`,第 16–17 字节就是 `e_type`,`int` 在不同平台上可能是 2 字节也可能是 4 字节,`long` 在 LP64 模型下是 8 字节、在 LLP64 下是 4 字节。我们要把磁盘上一段字节流**逐字段对齐**地读进来再解释,字段宽度差一个字节整个偏移就全错。`uint16_t` 永远是 2 字节、`uint64_t` 永远是 8 字节,这才是"格式"该有的样子。

> 外部依据:GCC 手册对 `<stdint.h>` 的说明见 [C Extensions / Standard Library](https://gcc.gnu.org/onlinedocs/gcc/);C 标准规定 `uintN_t` 在支持它的平台上"恰好 N 位、无填充、纯二进制表示"。

### 2. 数组退化与"指针即地址"

C/C++ 的数组名在绝大多数表达式里会**退化(decay)**成指向首元素的指针。内核里把内存映射表传给内核时就吃这个特性:

```cpp
// boot/boot_info.h(节选)
typedef struct {
    uint64_t base;    // Physical base address
    uint64_t length;
    uint32_t type;    // 1=usable, 2=reserved ...
    uint32_t acpi;
} __attribute__((packed)) MemoryMapEntry;

typedef struct {
    ...
    uint32_t       mmap_count;
    uint32_t       _pad;
    MemoryMapEntry mmap[32];   // 内嵌数组,32 个表项
} __attribute__((packed)) BootInfo;
```

`BootInfo` 里 `mmap[32]` 是一个**内嵌**数组(不是指针),所以整个 `BootInfo` 是一块连续内存——这正是 bootloader 和内核之间能传它(往固定物理地址 `0x7000` 一放、内核按指针一读就行)的前提。注意它和"数组当参数传"的区别:函数形参写 `MemoryMapEntry mmap[]` 会立刻退化成 `MemoryMapEntry*`,数组长度信息丢失,所以 `BootInfo` 才要额外存一个 `mmap_count` 来记录有效表项数。内核遍历时:

```cpp
for (uint32_t i = 0; i < info->mmap_count; i++) {
    auto& e = info->mmap[i];   // mmap[i] ≡ *(info->mmap + i)
    if (e.type == 1) { /* usable RAM */ }
}
```

`info->mmap[i]` 和 `*(info->mmap + i)` 完全等价——这就是"指针就是地址、下标就是地址算术"。

### 3. `BootInfo` 跨编译器的一致性:`_pad` 与 `static_assert`

`boot/boot_info.h` 有个特殊使命:**bootloader 用 `-m32` 编译、内核用 `-m64` 编译,两端要解释同一块内存**。所以它必须:

- 字段全用定宽类型(没有 `long`、没有 `size_t`);
- 显式加 `_pad` 填充,不依赖编译器的默认对齐;
- 整体 `__attribute__((packed))`;
- 末尾用 `static_assert` 把总大小钉死。

```c
// boot/boot_info.h:99-105
static_assert(sizeof(BootInfo) == 824, "BootInfo size mismatch");
```

这一行让任何一边改了字段却忘了算大小、导致 824 变成 820 时,**编译期**就报错——而不是等到运行时 bootloader 把 `mmap` 写歪、内核读出乱七八糟的内存才崩。`packed` 配 `static_assert` 是内核里最值钱的组合拳之一,下面专门讲。

## 二、位运算、位域与 `__attribute__((packed))`:把硬件格式拼出来

### 1. 页表项:位域把 64 位切成一段段

x86_64 的一个页表项(PTE)是 64 位,每一位都有意义:第 0 位 present、第 1 位 writable、……、第 12–51 位是物理页地址、第 63 位 NX。Cinux 用一个 **union + 位域** 把它表达出来:

```cpp
// kernel/arch/x86_64/paging.hpp:24-48
union PageEntry {
    uint64_t raw;                    // 整体当成一个 64 位数

    struct {                          // 或者按位域切成一段段
        uint64_t present  : 1;        // bit 0
        uint64_t writable : 1;        // bit 1
        uint64_t user     : 1;        // bit 2
        uint64_t pwt      : 1;
        uint64_t pcd      : 1;
        uint64_t accessed : 1;
        uint64_t dirty    : 1;
        uint64_t huge     : 1;
        uint64_t global   : 1;
        uint64_t _avail   : 3;
        uint64_t addr     : 40;       // bit 12..51:物理页帧地址
        uint64_t _avail2  : 11;
        uint64_t nx       : 1;        // bit 63:No-Execute
    };

    uint64_t phys_addr() const { return raw & ADDR_MASK; }
    void set_phys_addr(uint64_t phys) { raw = (raw & ~ADDR_MASK) | (phys & ADDR_MASK); }
    bool is_present() const { return (raw & FLAG_PRESENT) != 0; }
};
```

这里有个**关键设计**:同一个 8 字节,既能当整体 `raw` 处理(用来做位掩码运算),又能按位域逐位读写。`present : 1` 表示这个字段占 1 位。两种"看法"通过 union 共享内存。

为什么 `set_phys_addr` 用掩码而不用位域 `addr = phys`?因为位域赋值是**截断**——`addr` 只有 40 位,而 `phys` 是 64 位的物理地址,赋值时高位会被默默丢掉、低位对齐规则也依赖编译器实现。涉及"地址"这种跨字节、需要先右移 `>> 12` 再写入的高频操作,内核更愿意用显式的 `&` / `|` 位运算,**不依赖位域的位序约定**。位域用来表达"这一位是 present 标志"这种语义最舒服,但别拿它做精确的地址算术。

> 外部依据:GCC 手册 "When is a Volatile Object Accessed?" 一节明确警告——**位域的存储单元边界由实现决定,相邻位域可能只被部分访问,因此不建议用 volatile 位域访问硬件**(OSDev 同样反对把 MMIO 寄存器定义成位域)。Cinux 的 `PageEntry` 位域是非 volatile 的软件侧结构,所以安全。

### 2. `__attribute__((packed))`:禁止插入对齐填充

位域之外,内核还有大量"内存里逐字节排好的"硬件/文件结构,它们对填充零容忍——多塞一个字节的填充,后面所有字段的偏移全错。这时候就上 `__attribute__((packed))`,GCC 的 C++ 等价写法是 `[[gnu::packed]]`:

```cpp
// kernel/drivers/ahci/ahci_config.hpp:134-156
struct [[gnu::packed]] HBAPort {
    volatile uint32_t clb;        // 0x00:Command List Base Address (low 32)
    volatile uint32_t clbu;       // 0x04
    volatile uint32_t fb;         // 0x08
    ...
    volatile uint32_t rsv0;       // 0x1C:Reserved
    volatile uint32_t tfd;        // 0x20
    volatile uint32_t sig;        // 0x24
    volatile uint32_t ssts;       // 0x28:SATA Status
    volatile uint32_t sctl;       // 0x2C
    ...
    volatile uint32_t vendor[4];  // 0x70-0x7F
};
static_assert(sizeof(HBAPort) == 0x80, "HBAPort must be 128 bytes");
```

`packed` 的作用:告诉编译器**别在这结构体的成员之间、以及末尾加任何对齐填充**——成员一个挨一个。AHCI 规范定死了每个端口寄存器块是 `0x80`(128)字节,寄存器偏移精确到 `0x00/0x04/0x08/...`,差一个填充字节,你往 `0x2C` 写 SATA Control 写到的就是错的寄存器,磁盘就静默地不动。

`packed` 有代价:**结构体整体不再按最大成员对齐,访问未对齐的成员在有些架构上是慢的、甚至是异常**。x86_64 的未对齐访问能用但有性能损失;但对 MMIO/文件格式这种"内存布局是规范、不能动"的场合,没得选,必须 packed。

> 外部依据:GCC 手册 [Type Attributes / `packed`](https://gcc.gnu.org/onlinedocs/gcc/Common-Type-Attributes.html#index-packed-attribute) 描述了 packed 的语义与未对齐访问代价。AHCI 寄存器偏移见 Intel AHCI Specification rev 1.3(本仓库源码注释同引)。

## 三、volatile 与 memory barrier:它能挡什么、挡不住什么

这一节是整章最容易踩坑的地方,也是内核 bug 高发区。

### 1. volatile 的本职:挡编译器优化,不挡 CPU

`volatile` 告诉编译器:**对这个对象的每次访问都要老老实实发生,别缓存到寄存器、别合并、别消除**。内核里所有 MMIO 寄存器都标了 volatile:

```cpp
// kernel/drivers/ahci/ahci_config.hpp —— 整列寄存器都是 volatile uint32_t
volatile uint32_t ci;        // 0x38:Command Issue
```

没有 volatile 会怎样?设想一个轮询循环 `while ((port->tfd & 0x88) != 0) ;`,编译器一旦发现循环体里 `port->tfd` 没被改写,可能把它一次性读进寄存器、然后死循环寄存器里的值——而真正磁盘状态变了你也永远等不到。volatile 保证每次循环都真正去读内存(对 MMIO 来说就是真正去读硬件)。

Cinux 在画 framebuffer 时也靠 volatile 保证每次写都真正落到显存:

```cpp
// kernel/drivers/canvas.cpp:153
auto* dst = reinterpret_cast<volatile uint8_t*>(front_buf_->data());
```

### 2. volatile 挡不住的:跨对象乱序与 CPU 乱序

volatile 的致命错觉是以为它"等于内存屏障"。它**不是**。GCC 手册白纸黑字:

> "You cannot use a volatile object as a memory barrier to order a sequence of writes to non-volatile memory." —— GCC Manual, §6.10 "When is a Volatile Object Accessed?"

手册里举的例子(直译内核常见写法):

```cpp
int *ptr = something;
volatile int vobj;
*ptr = something;     // 写非 volatile 内存
vobj = 1;             // 写 volatile
// 不保证:*ptr 的写一定先于 vobj 的写"对外可见"
```

两层原因:

1. **编译器层**:对非 volatile 对象的访问,不和 volatile 访问排序。编译器可能先把 `*ptr` 的写延迟。
2. **CPU 层**:就算编译器按源码顺序生成了指令,x86_64 也会对 store 缓冲做重排(store–store 在 x86 上总体较强,但 load–store、以及跨核可见性仍需要屏障来保证顺序语义)。volatile 对 CPU 一无所知。

所以当你要"先写好命令结构、再置位 Command Issue 让硬件开干"这种**有先后依赖**的操作,光靠 `ci` 是 volatile 远远不够——你需要在两者之间插一道**编译器屏障**:

```cpp
// 编译器屏障:挡住编译器把两边的访存打乱
asm volatile("" ::: "memory");
```

空指令、clobber 列表写 `"memory"`,GCC 读懂为"这块内联汇编可能读写任意内存",于是它**不敢**把屏障前的内存操作挪到后面、也不敢把后面的挪到前面。这正是 Cinux 里到处出现的 `:::"memory"`:

```cpp
// kernel/arch/x86_64/io.hpp:33 —— port I/O 配 memory clobber
__asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
// kernel/arch/x86_64/irq.hpp:43
__asm__ volatile("cli" ::: "memory");
// kernel/arch/x86_64/paging.hpp:56-58 —— 刷 TLB
inline void flush_tlb(uint64_t virt) {
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}
```

`outb` 之后写 MMIO、`invlpg` 之后的访存、`cli`/`sti` 之后的临界区访问——全都靠 `"memory"` clobber 充当编译器屏障,确保"屏障前的写先完成、屏障后的读写不被提前"。

### 3. 编译器屏障 vs CPU 屏障:分工一句话

把两者的边界记清楚:

| 层 | 工具 | 挡什么 |
|---|---|---|
| 编译器 | `volatile` | 缓存到寄存器、消除、合并同一对象的访问 |
| 编译器 | `asm volatile("":::"memory")` | 编译器把屏障两侧的**任意**内存操作重排 |
| CPU | `mfence`/`lfence`/`sfence`、`lock` 前缀 | CPU 自己的乱序执行与跨核可见性 |

Cinux 跑在 x86_64 上,而 x86_64 的内存模型(TSO)对绝大多数 store–store、load–load 顺序已经够强,所以内核里**主要靠编译器屏障 + `volatile` 就够用**,只有在少数需要全屏障的场合(自旋锁的 `lock` 前缀等)才动用 CPU 屏障。但这个"够用"是**架构相关的**——换到 ARM/RISC-V 上,光这两样远远不够,必须显式 CPU 屏障。这也是为什么 OSDev 反复强调"volatile 不是线程同步原语"。

> 外部依据:[GCC Manual §6.10](https://gcc.gnu.org/onlinedocs/gcc/Volatiles.html)(volatile 的限度、`asm volatile("":::"memory")` 的用法);[OSDev Wiki — Memory Map (x86)](https://wiki.osdev.org/Memory_Map_(x86)) 与 MMIO 实践页对"volatile 不提供内存屏障、不可作线程同步"的告诫。x86_64 内存序(TSO)细节见 Intel SDM Vol.3A §8.2。

## 四、编译期钉布局:`static_assert` 与 `offsetof`

### 1. `static_assert`:把"我赌它有多大"变成编译错

前面已经看到 `static_assert(sizeof(BootInfo) == 824, ...)`、`static_assert(sizeof(PageEntry) == 8, ...)`、`static_assert(sizeof(HBAPort) == 0x80, ...)`。它们的作用一致:**布局一变就当场编译失败,绝不拖到运行时**。内核里这类断言几乎贴在每一个 packed 结构体后面:

```cpp
// kernel/proc/elf_types.hpp:77
static_assert(sizeof(Elf64_Ehdr) == 64, "Elf64_Ehdr must be 64 bytes");
// kernel/proc/elf_types.hpp:102
static_assert(sizeof(Elf64_Phdr) == 56, "Elf64_Phdr must be 56 bytes");
// kernel/mm/heap.hpp:45
static_assert(sizeof(BlockHeader) == 32, "BlockHeader must be 32 bytes");
```

C 里(`boot_info.h`)则用 C11 的 `_Static_assert`(C++ 里是 `static_assert`,Cinux 用 `#if defined(__cplusplus)` 分流),逻辑一样。把魔数大小写进断言,等于把"外部规范要求它必须 64 字节"这条契约固化在代码里,谁动谁知道。

### 2. `offsetof`:钉字段偏移,对齐汇编

上下文切换要靠一段汇编保存/恢复寄存器到 `CpuContext`。汇编是按**固定偏移**去读结构体里的 `r15`/`rbp`/`rsp` 的,所以每个字段的偏移必须和汇编约定完全一致。Cinux 用一串 `offsetof` 断言把这点钉死:

```cpp
// kernel/proc/process.hpp:68-91
struct alignas(16) CpuContext {
    uint64_t r15;       // 被调用者保存寄存器
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t rsp;
    uint64_t rip;
    uint64_t gs_base;
    uint64_t kgs_base;  // 共 80 字节,16 字节对齐
};

static_assert(offsetof(CpuContext, r15) == 0,  "r15 at offset 0");
static_assert(offsetof(CpuContext, rbp) == 32, "rbp at offset 32");
static_assert(offsetof(CpuContext, rsp) == 48, "rsp at offset 48");
static_assert(offsetof(CpuContext, rip) == 56, "rip at offset 56");
static_assert(offsetof(CpuContext, kgs_base) == 72, "kgs_base at offset 72");
static_assert(sizeof(CpuContext) == 80, "CpuContext must be 80 bytes");
```

`offsetof(Type, member)` 返回 `member` 在 `Type` 里的字节偏移(标准库 `<stddef.h>` 提供)。它的实现很巧妙:`&((Type*)0)->member`——把 0 当成一个 `Type*` 去取成员地址,得到的"地址"天然就是偏移量。

为什么 Cinux 只保存 `r15/r14/r13/r12/rbp/rbx` 这几个加 `rsp/rip`?因为**上下文切换发生在已知的函数调用边界**:调用者保存寄存器(rax/rcx/rdx/rsi/rdi/r8–r11)在被调函数里本就允许被破坏,根本不需要我们保存。这正是 `CpuContext` 只列这几个寄存器的根本原因——不是偷懒,是 ABI(System V AMD64)决定了的。`gs_base`/`kgs_base` 是为了每个任务的 GS 基址切换(见 per-CPU 那条路径)。这块汇编细节正文 004+ 会展开,这里只需要明白 `offsetof` 在替汇编**保证偏移正确**。

## 五、内核相关的 C 常见陷阱

只挑在内核里真的会咬人的几个,不展开成 C 语言大全。

### 1. 声明 vs 定义:头文件里写 `extern`,别写实体

`extern PerCPU g_per_cpu;` 是**声明**(告诉编译器有这么个全局变量,定义在别处),不带 `extern` 的 `PerCPU g_per_cpu;` 是**定义**(分配存储)。头文件里如果误写了定义,被多个 `.cpp` include 就会链接器报"重复定义"。Cinux 的做法:

```cpp
// kernel/proc/per_cpu.hpp:22
extern PerCPU g_per_cpu;   // 声明
// kernel/mm/heap.hpp:134
extern Heap g_heap;        // 声明
```

实体定义放在对应的 `.cpp` 里。内核里很多"全局单例"都走声明 + 定义分离这套,而不是用全局构造函数(内核启动早期根本还没跑 C++ 运行时,构造函数没机会执行——这也是为什么内核里你看不到 STL 和全局对象)。

### 2. 严格别名(Strict Aliasing):别拿 `uint32_t*` 去指 `uint64_t`

C/C++ 规定一个对象只能通过"兼容类型的指针"或 `char*`/`std::byte*` 访问。下面这种写法在 `-O2` 下是**未定义行为**,编译器可能优化出错:

```cpp
uint64_t x = 0;
uint32_t* p = (uint32_t*)&x;   // 严格别名违规
p[0] = 1; p[1] = 2;            // 编译器可能不按你预期刷新 x
```

内核要"换着宽度看同一块内存"时,正确做法是用 **union**(像 `PageEntry` 那样,`raw` 和位域共享内存,这是合规的)或者走 `memcpy`。位域 + union 的设计本身就顺手规避了严格别名问题——这也是 Cinux 页表项用 union 的另一个隐藏好处。

### 3. `if (x = y)`:把赋值当条件

```cpp
if (task = current_task) { ... }   // 你想写 ==
```

这是教科书级老坑,但内核里 `Task*` 指针满天飞,`if (task = lookup())` 把查找结果赋值并判断非空**本身是合法且常见的写法**,所以这种笔误特别容易混进去。内核没救你的只有代码审查和 `-Wall -Wextra`(GCC 对 `if (x = y)` 会 warn)。养成习惯:比较常量写在左边 `if (nullptr == task)` 能挡住一部分,但内核代码风格更认直接写清楚两步赋值。

### 4. 整数提升:`uint8_t` 算术别忘它会变 `int`

`uint8_t a = 200, b = 100; auto c = a + b;` 里 `c` 不是 `uint8_t` 而是 `int`(整型提升)。位运算尤其要小心:`~(uint8_t)0x0F` 不是 `0xF0`,而是 `0xFFFFFFF0`(32 位 int 取反),赋回 `uint8_t` 时虽然截断成 `0xF0`,但中间参与掩码运算时宽度早已不是 8 位。`PageEntry` 里所有位域都声明成 `uint64_t : N` 正是为了让位运算在 64 位宽度下进行,避免提升导致的意外。

---

### 参考

- Intel SDM Vol.3A — §4.5(4 级页翻译与 PTE 位定义,present/writable/addr/NX)、§8.2(内存序与屏障,mfence/lfence/sfence、TSO 模型)。本仓库已带本地 PDF `document/reference/intel/`(2023-06 版)。
- [GCC Manual — §6.10 When is a Volatile Object Accessed?](https://gcc.gnu.org/onlinedocs/gcc/Volatiles.html)(volatile 限度、`asm volatile("":::"memory")` 作编译器屏障、不要用 volatile 位域访问硬件)。
- [GCC Manual — Common Type Attributes (`packed`)](https://gcc.gnu.org/onlinedocs/gcc/Common-Type-Attributes.html#index-packed-attribute)。
- [OSDev Wiki — Memory Map (x86)](https://wiki.osdev.org/Memory_Map_(x86))(MMIO 与 bootloader/kernel 交接地址约定)。volatile 不可作线程同步、需显式屏障的告诫见 OSDev "When is a Volatile Object Accessed?" 相关社区讨论。
- Intel AHCI Specification rev 1.3(HBAPort/HBAMem 寄存器偏移;本仓库 `kernel/drivers/ahci/ahci_config.hpp` 注释同引)。
- 本仓库源码:[elf_types.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/elf_types.hpp)、[paging.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/paging.hpp)、[boot_info.h](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/boot_info.h)、[ahci_config.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/ahci/ahci_config.hpp)、[per_cpu.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/per_cpu.hpp)、[process.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/process.hpp)、[heap.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/heap.hpp)、[io.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/io.hpp)、[canvas.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/canvas.cpp)。

> 参考 URL 的有效性会在全局审查阶段用 open-websearch(bing)统一核活,与本系列其它章节一致。
