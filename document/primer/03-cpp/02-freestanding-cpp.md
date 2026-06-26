---
title: 02 · freestanding C++ 子集
---

# 02 · freestanding C++ 子集:内核敢用 C++ 的全部前提

> 这一章回答一个问题:为什么 Cinux 大内核敢用 C++ 写,却几乎不靠 C++ 运行时?——靠的不是"写法小心",而是把编译选项、运行时 stub、内联汇编、链接脚本四样东西**配合到位**。读完你能看懂正文 004 起的内核启动与堆内存代码,知道每一处 `extern "C"`、每一条链接脚本符号背后在补谁的窟窿。

## 这一章我们在补什么窟窿

正文里 Cinux 的引导链是这样递进的:实模式 MBR → Stage2 → mini 内核 → **big 内核**(真正的 C++ 内核)。big 内核是用 C++ 写的,但它是 `-ffreestanding -nostdlib` 编译链接的——也就是说,**标准库(libc / libstdc++)一个符号都不给**。

可问题是 C++ 编译器生成的代码,会偷偷引用一堆它"以为运行时一定会提供"的符号:

- `new` 一个对象,编译器去找 `operator new`;
- 写了个带纯虚函数的类,万一调用到纯虚,编译器埋了 `__cxa_pure_virtual`;
- 函数里有个 `static` 局部变量,编译器为了线程安全会调 `__cxa_guard_acquire/release`;
- 有全局对象带构造函数,这些构造指针被收进 `.init_array` 段,需要有人去遍历调用……

在用户态这些全由 `libstdc++` / `libc` 兜底;在内核里**没有任何人兜底**。只要漏一个符号,链接器就 `undefined reference`,内核根本链不出来。所以这章的全部工作就是:**把编译器期望的、但运行时没提供的那几个符号,自己手写补齐**,让 big 内核既享受 C++ 的封装,又不被运行时拖累。

这一章的代码主线就一个文件:[crt_stub.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/crt_stub.cpp)(`crt` = C runtime),外加 [CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/CMakeLists.txt) 里那串编译选项和 [linker.ld](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/linker.ld) 提供的符号。

## 1. freestanding 标志:这些选项各自砍掉了什么

big 内核的编译选项在 `kernel/CMakeLists.txt:21-31` 一字排开:

```cmake
set(BIG_KERNEL_COMPILE_OPTIONS
    -ffreestanding          # 不假设有宿主环境(标准库/OS)
    -fno-exceptions         # 不要异常机制
    -fno-rtti               # 不要运行时类型信息
    -fno-pie                # 不生成位置无关代码(我们按绝对高半地址链接)
    -fno-stack-protector    # 不要栈金丝雀(canary)
    -mcmodel=kernel         # 用内核代码模型(地址在地址空间高半)
    -mno-red-zone           # 禁用 x86-64 红区
    -Wall -Wextra
)
```

逐条说为什么——每一条都不是凑数,而是对应一个"内核里用不起"的机制:

- **`-ffreestanding`**:告诉编译器"这是独立环境"。标准库头文件只剩下 `<stddef.h>`、`<stdint.h>`、`<new>` 这类"不需要宿主"的 freestanding 子集。`memcpy`/`memset` 这些 Cinux 得自己在 `lib/` 里实现。GCC 手册里这一条把宿主式假设(`main` 入口、`atexit`、`<stdio.h>` 等)整片拿掉。

- **`-fno-exceptions`**:禁用 C++ 异常。异常展开(unwind)需要一整套 DWARF `.eh_frame` 表和 `libgcc` 的 `__cxa_*` 运行时,内核没有;异常抛出时还要在栈上回溯找 catch,中途栈帧可能被破坏——内核里出这种事就是直接死。关掉它,编译器既不生成 unwind 表,也不会引用 `_Unwind_*` 符号。链接脚本里也顺势 `/DISCARD/` 掉了 `.eh_frame`(见 `linker.ld:117`)。

- **`-fno-rtti`**:关掉 `typeid` / `dynamic_cast` 背后的类型信息。RTTI 数据(`typeinfo` 对象)体积可观,内核用不上多态运行时类型识别——Cinux 只用静态类型 + 我们自己控制的接口,不需要这层。

- **`-fno-stack-protector`**:不开栈金丝雀。栈保护会在每个函数栈帧里插一个随机 canary,返回前检查;若被破坏就调 `__stack_chk_fail`。内核有自己的页保护(boot 栈下面那块 `__boot_guard` unmapped 区,见 `linker.ld:97-101`),不重复搞这套。即便如此 `crt_stub.cpp:54` 仍写了 `__stack_chk_fail` 兜底。

- **`-mcmodel=kernel`** + **`-fno-pie`**:这对要一起看。x86-64 的"小代码模型"假设所有符号地址都在低 2GB,用 `mov` 配 32 位立即数就能取到地址。但 Cinux 内核链接在高半地址 `0xFFFFFFFF80000000`(`linker.ld:32`),超出 32 位范围。`-mcmodel=kernel` 专给这种"内核在地址空间最高 2GB"的布局设计,允许编译器用 31 位符号偏移生成正确的取址指令;`-fno-pie` 保证按绝对地址链接,不做位置无关重定位。

- **`-mno-red-zone`** ⭐:这条最致命。x86-64 System V ABI 给叶函数(不调别人的函数)留了个**红区**——`%rsp` 之下 128 字节可以不动栈指针就直接用。可中断是异步的,硬件中断/异常一来,CPU **直接从当前 `%rsp` 往下**压入 `SS:RSP:RFLAGS:CS:RIP`(可能还有错误码),根本不管红区——红区里的数据当场被覆盖(`syscall` 不走这套压栈:它在 `%rcx`/`%r11` 存返回点、由内核从 per-CPU 结构显式换栈,所以红区这条理由严格只针对中断/异常)。用户态有内核栈切换兜着,内核自己就在 Ring 0,中断一来写的就是红区。所以内核代码**必须** `-mno-red-zone`,否则一个中断就能踩烂正在用的局部变量。

> 外部依据:GCC 手册「Options for Code Generation Conventions」逐条描述了 `-ffreestanding` / `-fno-exceptions` / `-fno-rtti` / `-fstack-protector` 的语义;`-mcmodel` / `-mno-red-zone` 见 GCC 手册「x86 Options」。红区 128 字节及"中断处理器不尊重红区"出自 System V AMD64 ABI PS,亦见 Intel SDM Vol.3A 对中断栈压入的描述。

## 2. freestanding 运行时 stub:让链接成功的硬前提

这是本章的重头。即便编译选项砍干净了,编译器为合法 C++ 生成的代码仍会引用几个运行时符号。我们得在 `crt_stub.cpp` 里把它们一个不漏地补上——**漏一个就 `undefined reference`,内核链不出来**。这就是为什么这些 stub 是"能链接成功"的硬前提。

文件用 `extern "C"` 包住(`crt_stub.cpp:24`),因为这些符号是编译器按 C 链接(不 name-mangle)去找的。

### 2.1 `__cxa_pure_virtual`:调用到纯虚时的"停机"

```cpp
// crt_stub.cpp:36
[[noreturn]] void __cxa_pure_virtual() {
    __asm__ volatile("cli");
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)'V'), "Nd"((uint16_t)0xE9));
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

如果一个抽象基类的纯虚函数被实际调用(通常是因为在构造/析构期间误调,或虚表没填好),编译器埋的虚表项会指向 `__cxa_pure_virtual`。用户态它 `abort()`;内核里我们不打印堆栈(没有栈展开),直接往 QEMU 调试端口 `0xE9` 吐一个 `'V'` 字符(开 `-debugcon` 就能在串口看到),然后关中断死循环——`[[noreturn]]` 告诉编译器这个函数绝不返回,省掉调用处无意义的返回路径。

### 2.2 `__stack_chk_fail`:栈金丝雀被破坏

```cpp
// crt_stub.cpp:54
[[noreturn]] void __stack_chk_fail() {
    __asm__ volatile("cli");
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)'S'), "Nd"((uint16_t)0xE9));
    while (1) { __asm__ volatile("cli; hlt"); }
}
```

虽然 `-fno-stack-protector` 默认不开,但万一有人局部开了栈保护(或某些库代码自带),这个符号就必须在——它就是 canary 校验失败时的回调。同样吐 `'S'` 后停机。

### 2.3 `__cxa_atexit` / `__dso_handle`:内核永不退出

```cpp
// crt_stub.cpp:73
int __cxa_atexit(void (*)(void*), void*, void*) { return 0; }

// crt_stub.cpp:89
void* __dso_handle = nullptr;
```

C++ 里带析构的全局/静态对象,构造时会被编译器通过 `__cxa_atexit(func, arg, dso)` 登记一个"退出时回调"。内核**永远不退出**,所以这个登记就是空操作——返回 0 表示"成功,我记下了"(其实啥也没干)。`__dso_handle` 是"当前动态共享对象(DSO)的句柄",用来区分不同模块的析构;内核是个单一大 ELF、没有动态加载,所以一个 `nullptr` 就够。

这两个是配对出现的:`__cxa_atexit` 的第三个参数就是 `__dso_handle`。

### 2.4 `__cxa_guard_acquire` / `__cxa_guard_release`:函数局部 static 守卫

```cpp
// crt_stub.cpp:109
int __cxa_guard_acquire(uint64_t* guard) {
    if (*guard != 0) return 0;   // 已初始化过
    return 1;                    // 没初始化,去初始化吧
}

// crt_stub.cpp:122
void __cxa_guard_release(uint64_t* guard) { *guard = 1; }
```

这是 Itanium C++ ABI 规定的函数局部 `static` 初始化协议。你写:

```cpp
void f() {
    static Logger logger;   // 第一次进 f() 才构造
}
```

编译器不会直接调构造函数,而是给 `logger` 配一个 64 位**守卫变量**(guard),生成这样的展开代码(伪码):

```text
if (__cxa_guard_acquire(&guard) != 0) {   // 还没初始化?
    构造 logger;                            // 真正调 Logger 构造函数
    __cxa_guard_release(&guard);            // 标记:初始化完成
}
```

在多线程用户态,`acquire` 内部要做原子操作加锁,保证 `logger` 只被构造一次。Cinux 现在单核、且 boot 阶段没有抢占,所以守卫只起"标记位"作用:`acquire` 看 guard 是不是 0,`release` 把它写成 1。语义完全对齐 Itanium ABI(guard 非 0 表示已初始化),只是省掉了锁。

> 外部依据:Itanium C++ ABI §2.8「Initialization Guard Variables」定义了 guard 变量的语义(0 = 未初始化,非 0 = 已初始化)和 `__cxa_guard_acquire/release/abort` 三个入口;§3.3「Construction and Destruction APIs」定义了 `__cxa_atexit` 的参数与 `__dso_handle` 的角色。GCC/Clang 在 x86-64 上都遵循该 ABI。

### 2.5 `_init_global_ctors`:遍历 `.init_array` 调全局构造函数

```cpp
// crt_stub.cpp:133-134  —— 链接脚本提供的两个符号(数组起止)
extern void (*__init_array_start[])();
extern void (*__init_array_end[])();

// crt_stub.cpp:143
void _init_global_ctors() {
    void (**start)() = __init_array_start;
    void (**end)()   = __init_array_end;
    for (void (**func)() = start; func != end; func++) {
        void (*ctor)() = *func;
        if (ctor != nullptr) { ctor(); }
    }
}
```

这是 stub 里**唯一需要我们自己主动调、而非被编译器引用**的。C++ 里全局对象(比如 `extern cinux::mm::Heap g_heap;`)的构造函数,不会自动跑——编译器只是把"调用这个构造函数"的函数指针,一个个塞进 `.init_array` 段。要有人去遍历这个段、逐个调用,全局对象才真正被构造起来。

这就是 `_init_global_ctors` 的活:从 `__init_array_start` 走到 `__init_array_end`,把每格里的函数指针取出来调一遍。它在 boot 阶段被调一次(清完 BSS 之后、进 `kernel_main` 之前),随后全局对象就都活过来了。

这两个边界符号从哪来?**链接脚本给的**——`linker.ld:63-67`:

```ld
.init_array : AT(ADDR(.init_array) - KERNEL_VMA) ALIGN(8) {
    __init_array_start = .;
    KEEP(*(.init_array .init_array.*))
    __init_array_end   = .;
}
```

`KEEP` 是告诉链接器"别因为没被引用就 GC 掉 `.init_array`"——它确实没有任何 C 代码引用,但 `_init_global_ctors` 会通过那两个边界符号间接用到它。我们会在第 5 节把这条链彻底串起来。

> 外部依据:OSDev「Calling Global Constructors」描述了遍历 `.init_array`(或老式 `.ctors`)调用全局构造函数的标准做法,以及为何 `KEEP` 必须保留该段。

## 3. 全局 `new` / `delete`:转给 `g_heap`,外加一点被允许的 STL 碎片

注意 [crt_stub.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/crt_stub.cpp) 第 157 行的注释:**`operator new/delete` 故意写在 `extern "C"` 块之外**(`crt_stub.cpp:165-230` 全在那块外面)。为什么?因为 `operator new` 是 C++ 重载,需要正常的 C++ name mangling——编译器找的就是 `operator new(unsigned long)` 这个 mangled 符号,不是 C 链接。

```cpp
// crt_stub.cpp:165
void* operator new(unsigned long size) {
    return cinux::mm::g_heap.alloc(static_cast<size_t>(size));
}

// crt_stub.cpp:179  —— placement/aligned 重载
void* operator new(unsigned long size, std::align_val_t align) {
    return cinux::mm::g_heap.alloc(static_cast<size_t>(size), static_cast<size_t>(align));
}

// crt_stub.cpp:193
void operator delete(void* ptr) noexcept {
    cinux::mm::g_heap.free(ptr);
}
```

普通 `new` 直接转给全局堆 `cinux::mm::g_heap`(`heap.hpp` 里 `extern Heap g_heap;`),它是个 first-fit + 合并的链表堆分配器(`alloc(size, align=16)` / `free(ptr)`)。`delete` 同理转 `g_heap.free`。全套重载(单对象/数组/sized/aligned)都补了,这样 `new`/`delete`/`new[]`/`delete[]` 在内核里都能用。

这里出现了一个**被允许的 STL 碎片**:`std::align_val_t` 和 placement new。`<new>` 头文件是 freestanding 子集的一部分(不需要宿主),所以可以放心 `#include <new>`(`crt_stub.cpp:20`)。`std::align_val_t` 是个枚举,用来给对齐版 `operator new` 传对齐要求(C++17 的扩展分配接口)。Cinux 在真正需要对齐的地方直接用 placement new,比如分配 TCB:

```cpp
// kernel/proc/fork.cpp:120
auto* child = new (std::align_val_t{alignof(Task)}) Task;
```

这句的意思是:按 `Task` 的自然对齐(`alignof(Task)`)去堆里分配,在原地构造一个 `Task`——它命中了上面那个 `operator new(size, align_val_t)` 重载,转给 `g_heap.alloc(size, align)`。这就是内核里"既能用对齐分配、又不引入容器/异常/RTTI"的克制做法。

> 这里的取舍是 Cinux freestanding C++ 子集的边界:允许 `new`/`delete`、placement new、`std::align_val_t`、`<new>`、`cstdint` 这类**不依赖运行时的语言/库碎片**;排除 STL 容器(`vector`/`string`)、异常、RTTI、智能指针、虚函数多态——那些要么拖运行时,要么和内核自己的内存/调度模型打架。详见正文 004 起对内核堆与 `Heap` 实现的展开。

## 4. 内联汇编边界:`asm volatile`、约束、clobber

内核免不了要直接发 CPU 指令(读端口、发 `cli`/`hlt`、走 `syscall`)。C++ 用 GCC 扩展内联汇编来做,基本形态见 [io.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/io.hpp):

```cpp
// io.hpp:31-35
inline uint8_t io_inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

// io.hpp:43-45
inline void io_outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
}
```

四个要点,每一个都有 why:

1. **`volatile`**:`asm volatile` 告诉编译器"别优化掉这段、也别把它和别的 `asm` 重排"。I/O 指令有副作用(动了设备状态),编译器要是觉得"反正返回值没用到"就给删了,驱动当场失效。

2. **输出/输入约束**:格式是 `asm(模板 : 输出 : 输入 : clobber)`。
   - `"=a"(value)`:`=` 表示输出,`a` 表示绑定到 `%al/%ax/eax/rax`(以变量宽度为准)。`inb` 把读到的字节放 `%al`,`value` 因此拿到结果。
   - `"a"(value)`(输出侧没有 `=` 是输入):写端口时把 `value` 放进 `%al`。
   - `"Nd"(port)`:`N` 表示"常量立即数(0-255)能塞进单字节",`d` 表示"也可放 `%edx/%dx/%dl`"。`out %al, %dx` 的端口既可以是立即数(如 `0x80`)也可以是 `%dx` 里的值,这个约束两种都允许。模板里的 `%0`/`%1` 就是按顺序引用这些操作数(AT&T 语法源在左、目的在右,所以 `inb %1, %0` 是"从端口 `%1` 读进 `%0`")。

3. **`"memory"` clobber**:这是最容易被忽略的一条。它告诉编译器"这段汇编可能读写内存,你别把前后的内存访问跨过它重排"。`in`/`out` 是同步化操作,前后往往有对设备 MMIO 内存的读写;不加 `"memory"`,优化器可能把一次"写命令寄存器 → 读状态寄存器"重排成"读 → 写",驱动逻辑就错了。所以 io.hpp 里**所有** I/O 函数都带 `"memory"`,当作编译屏障用。

4. **`syscall` 路径的两个隐式 clobber**:在 `syscall.hpp` 描述的 `SYSCALL` 入口里,`syscall` 指令本身会破坏 `%rcx`(被返回地址覆盖)和 `%r11`(被 RFLAGS 覆盖)——这是 CPU 定死的,不是 ABI 约定。所以任何把 `syscall` 包进 `asm` 的写法,clobber 列表都得写 `"rcx", "r11"`,再加上 `"memory"`,凑成 `rcx,r11,memory`。这正是 `io.hpp` 的 `"memory"` 思路在更复杂场景下的扩展:把"会被破坏但没列进输出/输入"的寄存器和内存如实告诉编译器。

> 外部依据:GCC 手册「Extended Asm」描述了 `volatile`、约束字符(`a/N/d/=`)、clobber 列表(含 `"memory"` 作为编译屏障)的语义。`SYSCALL` 破坏 `%rcx`/`%r11` 出自 Intel SDM Vol.2B 的 `SYSCALL` 指令说明(详见正文 syscall 章节)。

## 5. `extern "C"` 边界:把汇编符号、C 符号、链接脚本符号三处串起来

最后一节,我们用一个完整的链路把前面的东西串起来。内核里有三类"跨语言/跨阶段"的符号,都得靠 `extern "C"` + 链接脚本对齐:

**(a) C++ 调汇编写的入口。** `syscall.hpp:69-100`:

```cpp
extern "C" {
void syscall_entry();                    // 汇编里定义,C++ 这里声明
int64_t syscall_dispatch(uint64_t nr, uint64_t a1, ..., uint64_t a6);
}  // extern "C"
```

汇编源文件(`syscall.S`)里的符号是 C 链接的(没 mangle)。C++ 这边要让链接器找到 `syscall_entry`,就得把它放进 `extern "C"`——否则编译器去找的是 mangled 名(`_Z13syscall_entryv`),链不上。这是"接汇编符号"的典型用法。

**(b) C++ 提供给汇编/启动代码的 stub。** 第 2 节里 `__cxa_pure_virtual`、`__stack_chk_fail`、`_init_global_ctors` 这些,都用 `extern "C"`(`crt_stub.cpp:24`),因为引用它们的代码(编译器埋的桩、boot.S)都按 C 链接找符号。

**(c) 链接脚本定义的段边界符号。** 这是真正的"三处串起来"。`linker.ld` 提供了好几个这样的符号:

```ld
# linker.ld:64-66  —— .init_array 边界(第 2.5 节用过)
__init_array_start = .;
KEEP(*(.init_array .init_array.*))
__init_array_end   = .;

# linker.ld:89-90  —— 内核映像边界
__kernel_end = .;
PROVIDE(__kernel_size = __kernel_end - (KERNEL_VMA + KERNEL_LMA));
```

`crt_stub.cpp:133-134` 用 `extern` 声明 `__init_array_start/end` 这两个"数组",`_init_global_ctors` 遍历它;`__kernel_end`/`__kernel_size` 则是给 PMM(物理内存管理器)用的——它得知道"内核映像占到哪里",才能从后面开始分配物理页(详见正文 004 起的内存管理章节)。

**完整链路是这样的**:

```text
链接脚本 linker.ld          编译器/汇编生成的引用方          C++ 里补的实现
────────────────────       ─────────────────────         ─────────────────
__init_array_start ──┐     .init_array 段(编译器塞构造指针)   crt_stub.cpp:143
__init_array_end   ──┤  →  _init_global_ctors() 遍历   ←──  extern "C" 实现
                     │     boot.S 在 BSS 清零后调用它
__kernel_end  ───────┤     PMM 用来定分配起点           ←──  正文 004+ 使用
__kernel_size ───────┘
                     │
                     │     syscall_entry(汇编 SYSCALL 入口, syscall.S 定义)  syscall.hpp:83 声明
                     └──>  extern "C" 桥接            ←──  syscall.cpp:111 引用(写 LSTAR MSR)
```

三处缺一不可:**链接脚本**定义符号(它是唯一知道最终内存布局的角色)、**汇编/编译器生成的代码**按 C 链接去引用这些符号、**C++** 用 `extern "C"` 声明并补上实现。`.init_array` 这条尤其典型——它没有 C 代码直接引用,纯靠 `KEEP` 保留 + 边界符号 + `_init_global_ctors` 三者配合,全局构造才跑得起来。

> 这一段把"编译选项砍什么(第 1 节)→ 运行时补什么(第 2 节)→ 内存怎么分配(第 3 节)→ 怎么直接发指令(第 4 节)→ 三类符号怎么串(本节)"接成一个闭环。freestanding C++ 不是"少用点特性"那么简单,而是这五样东西的精确配合。

---

### 参考

- Itanium C++ ABI — §2.8「Initialization Guard Variables」(`__cxa_guard_acquire/release/abort` 的 guard 变量语义)、§3.3「Construction and Destruction APIs」(`__cxa_atexit` 参数与 `__dso_handle` 角色)、§2.9「RTTI」(`-fno-rtti` 关掉的东西):https://itanium-cxx-abi.github.io/cxx-abi/abi.html
- GCC 手册 — [Options for Code Generation Conventions](https://gcc.gnu.org/onlinedocs/gcc/Code-Gen-Options.html)(`-ffreestanding` / `-fno-exceptions` / `-fno-rtti` / `-fstack-protector`)、[x86 Options](https://gcc.gnu.org/onlinedocs/gcc/x86-Options.html)(`-mcmodel=kernel` / `-mno-red-zone`)、[Extended Asm](https://gcc.gnu.org/onlinedocs/gcc/Extended-Asm.html)(`volatile` / 约束字符 / `memory` clobber)。
- System V AMD64 ABI PS — 红区(叶函数 `%rsp` 之下 128 字节)与"中断/信号处理器不尊重红区"的约定,即 `-mno-red-zone` 在内核场景的根据。
- OSDev — [Calling Global Constructors](https://wiki.osdev.org/Calling_Global_Constructors)(遍历 `.init_array`/`.ctors` 调用全局构造、`KEEP` 保留该段);OSDev C++ 相关页对 `__cxa_*` stub 的社区总结(本机 OSDev 站反爬,以 Itanium ABI 原文为准)。
- 本仓库源码:[crt_stub.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/crt_stub.cpp)、[CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/CMakeLists.txt)、[linker.ld](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/linker.ld)、[io.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/io.hpp)、[syscall.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/syscall.hpp)、[heap.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/heap.hpp)、[fork.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/fork.cpp)。
- `__cxa_*` 与 `extern "C"` 链接可见性的背景,对照 [深入理解 C/C++ 的编译与链接技术(导论)](file:///home/charliechen/NoteBookProject/Computer_Science/%E7%A8%8B%E5%BA%8F%E8%AF%AD%E8%A8%80%E8%AE%BE%E8%AE%A1/C%26C%2B%2B/%E7%BC%96%E8%AF%91%E5%92%8C%E9%93%BE%E6%8E%A5/%E6%B7%B1%E5%85%A5%E7%90%86%E8%A7%A3CC%2B%2B%E7%9A%84%E7%BC%96%E8%AF%91%E4%B8%8E%E9%93%BE%E6%8E%A5%E6%8A%80%E6%9C%AF.md)(本地笔记,讲符号可见性与链接性的基础)。

> 参考 URL 的有效性会在全局审查阶段用 open-websearch(bing)统一核活,与本系列其它章节一致。
