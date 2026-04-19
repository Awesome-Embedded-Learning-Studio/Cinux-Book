# kprintf：在 freestanding 世界里，从零造一个内核格式化输出引擎

> 作者：
> 标签：x86-64, va_list, 格式化输出, freestanding C++, 串口驱动, 补码陷阱, 单元测试 Mock, 内核开发, OS 开发

---

## 前言

写内核写到这一步，我们有了 Bootloader，有了 mini kernel，有了 GDT/IDT/中断，甚至有了磁盘驱动和 ELF 加载器——但说实话，调试的时候一直靠 QEMU 的 `-d int` 日志和 `isa-debug-exit` 猜状态，这种开发体验真的谈不上友好。一个没有 printf 的内核就像一个没有日志的分布式系统，出了 bug 你两眼一抹黑，只能盯着 QEMU 窗口发呆。

在 freestanding 环境下没有标准库，自然也就没有 `printf`。这听起来是一件小事——不就是打印个字符串嘛？但当你真的开始动手，你会发现"在裸机上打印一个 -12345"这件事涉及的底层机制比你想象的多得多：可变参数怎么传递？有符号数怎么转字符串？十六进制怎么格式化？INT64_MIN 取负为什么会爆炸？这些问题每一个都值得单独拆开来讲，而这一章我们就要把它们全部走一遍。

所以这一章我们要做的，就是从零开始给大内核实现一个 `kprintf`——一个简化版的 `printf`，输出目标是通过串口驱动写到 COM1，最终在宿主机终端上显示。它不需要支持浮点、不需要支持 `%n`、不需要宽字符集，但它要能正确处理 `%d`、`%u`、`%x`、`%X`、`%p`、`%s`、`%c`、`%%` 这些常用格式符，还要有宽度修饰符和零填充。整个引擎用不到 300 行代码实现，但每一步都有值得聊的设计决策。

## 环境说明

实验环境和前面几章保持一致：x86_64 平台，GCC/G++ + CMake 构建，QEMU 模拟运行。内核使用 freestanding C++23 编译，无标准库、无异常、无 RTTI。kprintf 的输出目标是 COM1 串口（I/O 端口 0x3F8），在 QEMU 中通过 `-serial stdio` 参数将串口输出直接显示在终端上。串口驱动已经在上一章（009B）中实现，配置为 115200 波特率 8N1 轮询模式。kprintf 位于 `cinux::lib` 命名空间下，源文件在 `kernel/lib/kprintf.hpp` 和 `kernel/lib/kprintf.cpp`，对应的单元测试在 `tests/unit/test_kprintf.cpp`。

## 第一步——搞清楚 va_list 是怎么回事

在写 kprintf 之前，我们需要理解 `printf` 家族的核心机制：可变参数。当你写 `kprintf("val=%d, name=%s", 42, "hello")` 的时候，编译器是怎么把 "42" 和 "hello" 传给函数的？答案藏在 `<stdarg.h>` 里的 `va_list`、`va_start`、`va_arg`、`va_end` 这四个宏/类型中。

`va_list` 在 x86_64 的 System V ABI 下本质上是一个结构体，里面记录了可变参数的传递状态。x86_64 的调用约定规定，前 6 个整数参数通过寄存器（rdi、rsi、rdx、rcx、r8、r9）传递，前 8 个浮点参数通过 xmm0-xmm7 传递，剩下的参数通过栈传递。`va_list` 内部维护着一组指针和计数器，用来跟踪"当前该从哪个寄存器或栈位置取下一个参数"。`va_start` 初始化这个状态，`va_arg(args, type)` 根据类型大小从正确的位置取出参数并推进状态，`va_end` 做清理工作。

你可以把 `va_list` 理解为一个"参数游标"——它指向可变参数列表的当前位置，每次 `va_arg` 调用就像从游标处取出一个值然后前进一格。类型信息由 `va_arg` 的第二个参数提供，编译器根据这个类型来决定要读多少字节、怎么解释读出来的二进制数据。这也是为什么 `printf` 的格式字符串和实际参数类型必须匹配——如果格式字符串里写了 `%d` 但你传了一个 `double`，`va_arg` 会按 `int` 的大小去读，结果就是垃圾值。

理解了这些之后，我们就可以开始设计 kprintf 的接口了。和标准 C 库一样，我们提供三个公共函数：`kprintf(fmt, ...)` 是最常用的入口，接受格式字符串和可变参数；`kvprintf(fmt, va_list)` 是它的 `va_list` 变体，适用于需要把 `va_list` 透传给其他函数的场景；`kpanic(fmt, ...)` 是内核的"急停按钮"，打印消息后执行 `cli; hlt` 死循环。此外还有一个 `kprintf_init()` 函数，用来一次性初始化串口硬件，必须在所有其他调用之前执行。

```cpp
namespace cinux::lib {

void kprintf_init();
void kprintf(const char* fmt, ...);
void kvprintf(const char* fmt, va_list args);
[[noreturn]] void kpanic(const char* fmt, ...);

}  // namespace cinux::lib
```

你会发现这个接口设计和 glibc 的 `printf`/`vprintf`/`abort` 非常类似——这不是巧合，而是 C 语言库设计中一个成熟的模式：`printf` 和 `vprintf` 总是成对出现，区别只在于前者用 `...` 声明可变参数，后者直接接受 `va_list`。这种成对设计的好处是，如果你自己写了一个带日志级别的包装函数 `log_info(const char* fmt, ...)`，内部可以用 `va_start` 取到 `va_list`，然后传给 `kvprintf` 复用格式化逻辑，不需要再实现一遍。

## 第二步——OutputFn 模板：格式化引擎的核心抽象

接口定义好了，接下来要解决一个关键的架构问题：格式化引擎和输出后端之间怎么解耦？格式化引擎只负责把 `%d` 变成 `"42"`，但这个 `"42"` 最终要去哪里——串口？debug console？测试缓冲区？——格式化引擎不应该关心这些。

传统的做法是使用函数指针回调：格式化引擎接受一个 `void (*putc_fn)(char)` 的函数指针，每生成一个字符就调用它。这种方式很直观，Linux 内核的早期版本和很多嵌入式 RTOS 都这么干。但我们选择了另一种方式——C++ 模板参数。格式化引擎是一个模板函数，模板参数 `OutputFn` 是一个可调用对象（lambda、函数指针、仿函数都行），每次输出字符时直接调用它。

```cpp
template <typename OutputFn>
void vkprintf_impl(OutputFn&& putc_fn, const char* fmt, va_list args) {
    char buffer[64];
    // ... format string parsing loop ...
}
```

为什么要用模板而不是函数指针？核心原因是性能——编译器在编译期就知道 `OutputFn` 的具体类型，可以把 `putc_fn` 的调用完全内联到格式化循环里。在生产代码中，`putc_fn` 是一个捕获了串口引用的 lambda `[&](char c) { g_serial.putc(c); }`，编译器最终生成的机器码和直接在循环里写 `g_serial.putc(c)` 完全一样，没有任何函数调用的开销。而函数指针每次调用都需要一次间接跳转，在现代 CPU 流水线上代价不低——尤其是当你连续格式化几百个字符的时候，每个字符都要跳一次。

当然模板也有代价——每种不同的 `OutputFn` 类型都会实例化一份 `vkprintf_impl` 的代码。但在我们的场景中，这个函数只会被实例化两到三次（生产 lambda 加上测试用 mock），代码膨胀完全可以忽略。而且这种"零开销抽象"正是 C++ 模板设计的初衷：你写了抽象的通用代码，编译出来的二进制和你手写特化代码一样高效。

格式说明符方面，kprintf 支持 `%%`（百分号字面量）、`%c`（字符）、`%s`（字符串，`nullptr` 打印为 `"(null)"`）、`%d`（有符号十进制）、`%u`（无符号十进制）、`%x`/`%X`（十六进制，小写/大写）、`%p`（指针，固定 `0x` 前缀加 16 位十六进制）。宽度修饰符支持 `%Nd`（最小宽度 N，空格填充）和 `%0Nd`（零填充）。不支持左对齐、精度限定符和长度修饰符——在我们的内核场景下，这些功能的优先级很低。

## 第三步——数字格式化的底层算法

现在我们深入到格式化引擎的内部，看看数字是怎么变成字符串的。这一步涉及两个核心函数：`format_decimal` 处理十进制格式化（`%d` 和 `%u`），`format_hex` 处理十六进制格式化（`%x`、`%X` 和 `%p`）。

### 十进制：除法取余的经典算法

`format_decimal` 接受一个 `int64_t value`、一个输出缓冲区和缓冲区大小，返回写入的字符数（不包括结尾的 `\0`）。算法的核心是除法取余——不断对 10 取余得到最低位数字，然后除以 10 去掉最低位，直到值变为 0。

```cpp
static int format_decimal(int64_t value, char* buffer, int buffer_size) {
    if (buffer_size < 1) return 0;

    int  idx    = 0;
    bool is_neg = value < 0;

    if (is_neg) {
        if (value == (-9223372036854775807LL - 1)) {
            // INT64_MIN special case
            const char* min_str = "-9223372036854775808";
            int len = 0;
            while (min_str[len] != '\0' && idx < buffer_size - 1) {
                buffer[idx++] = min_str[len++];
            }
            buffer[idx] = '\0';
            return idx;
        }
        value = -value;
    }

    uint64_t abs_val = static_cast<uint64_t>(value);
    char     tmp[24];
    int      tmp_idx = 0;

    do {
        tmp[tmp_idx++] = '0' + static_cast<char>(abs_val % 10);
        abs_val /= 10;
    } while (abs_val > 0 && tmp_idx < 24);

    if (is_neg && idx < buffer_size - 1) {
        buffer[idx++] = '-';
    }

    while (tmp_idx > 0 && idx < buffer_size - 1) {
        buffer[idx++] = tmp[--tmp_idx];
    }
    buffer[idx] = '\0';

    return idx;
}
```

我们来逐步拆解这段代码。

首先是防御性检查——如果缓冲区大小小于 1，直接返回 0。这是为了防止后面所有操作在空缓冲区上越界写入，虽然正常情况下调用方总会传一个至少 64 字节的缓冲区，但在工程实践中，防御性检查永远不嫌多。

接下来是负数处理。如果 `value < 0`，我们把 `is_neg` 标记为 true，然后需要把 value 转为正数来执行除法取余。但这里有一个经典的补码陷阱，也是这一章最值得展开讲的坑点——

### INT64_MIN 的补码陷阱

对于 `int64_t` 来说，`INT64_MIN` 的值是 `-9223372036854775808`，它的绝对值 `9223372036854775808` 超出了 `int64_t` 的表示范围（最大正值是 `9223372036854775807`，即 `INT64_MAX`）。在补码表示下，`-INT64_MIN` 的结果还是 `INT64_MIN` 本身——因为 `9223372036854775808` 的二进制表示需要 64 位加一位符号位，溢出了一位，回绕到了负数范围。

第一次写的时候我完全没想到这个补码陷阱。测试的时候打印了个负数感觉没问题，直到有一天用 `%d` 打印了一个恰好是 `INT64_MIN` 的值，输出直接变成了一个莫名其妙的正数——因为 `-INT64_MIN == INT64_MIN`，取负之后值没变，后面的格式化逻辑把它当成正数处理了。这种 bug 非常隐蔽，因为大部分时候你不会恰好遇到 `INT64_MIN`，但一旦遇到就是完全错误的输出。

我们的处理方式直接而粗暴：检查 `value == (-9223372036854775807LL - 1)`，如果命中，直接把硬编码的字符串 `"-9223372036854775808"` 拷贝到缓冲区里返回。注意这里不能写 `-9223372036854775808LL`，因为字面量 `-9223372036854775808` 在某些编译器下会被解析为一元减号应用到超出 `long long` 范围的正数字面量上，导致编译警告甚至错误——正确的写法是 `(-9223372036854775807LL - 1)`。这不够优雅，但绝对正确，而且这种情况在实际运行中极其罕见。

排除了 `INT64_MIN` 之后，就可以安全地做 `value = -value`，然后 `static_cast` 为 `uint64_t` 进入核心的除法取余循环。循环体只有两行：`abs_val % 10` 得到最低位数字，转成字符 `'0' + digit` 存入临时数组 `tmp`；然后 `abs_val /= 10` 去掉最低位。注意这里用的是 `do { ... } while` 而不是 `while { ... }`——当 `value == 0` 时，`do-while` 至少执行一次循环体，产生 `'0'`。如果用 `while` 循环，0 这个值会产生空字符串，这是一个常见的边界 bug。

循环产生的字符是逆序的——比如 42 会先产生 `'2'` 再产生 `'4'`。所以我们在后面的循环中把 `tmp` 数组从后往前拷贝到输出缓冲区，实现正序输出。在写入正序数字之前，如果原数是负数，先在缓冲区头部写入 `'-'`。临时数组 `tmp` 的大小设为 24，因为 `int64_t` 的最大十进制位数是 19 位，加上可能的负号和结尾的 `\0`，24 个字节绰绰有余。

你会发现这种"逆序生成、正序拷贝"的模式是数值格式化算法的通用范式。先从最低位开始生成是因为取余操作只能方便地提取最低位，生成的是逆序结果，然后反转到正序。如果你去看 glibc 或者 Linux 内核的 `number()` 函数，会发现它们也用了完全相同的策略，只不过 Linux 内核额外处理了千分位分隔符和区域设置。

### 十六进制：位运算的四两拨千斤

`format_hex` 比 `format_decimal` 简单得多，因为十六进制天然适配二进制表示——每 4 个二进制位恰好对应一个十六进制数字。

```cpp
static int format_hex(uint64_t value, char* buffer, int buffer_size, bool lowercase) {
    if (buffer_size < 1) return 0;

    const char* digits = lowercase ? "0123456789abcdef" : "0123456789ABCDEF";
    char  tmp[20];
    int   tmp_idx = 0;

    do {
        tmp[tmp_idx++] = digits[value & 0xF];
        value >>= 4;
    } while (value > 0 && tmp_idx < 20);

    int idx = 0;
    while (tmp_idx > 0 && idx < buffer_size - 1) {
        buffer[idx++] = tmp[--tmp_idx];
    }
    buffer[idx] = '\0';

    return idx;
}
```

核心算法只有三行：`digits[value & 0xF]` 用位掩码取出最低 4 位，作为查找表的索引得到对应的十六进制字符；`value >>= 4` 右移 4 位去掉已经处理的位；重复直到 `value` 变为 0 或者临时数组满了。`digits` 数组根据 `lowercase` 参数选择小写或大写的查找表——这比 `if (digit < 10) '0' + digit else 'a' + digit - 10` 的分支判断更直接，而且编译器可以把它优化成一次数组访问。

`tmp` 数组大小是 20，因为 `uint64_t` 最多 16 个十六进制位，加上安全裕量。和 `format_decimal` 一样，产生的字符是逆序的，也要从后往前拷贝到输出缓冲区。

## 第四步——格式解析引擎：vkprintf_impl 的完整逻辑

底层格式化辅助函数就位之后，我们来看格式解析引擎 `vkprintf_impl` 的完整逻辑。这个函数遍历格式字符串，逐字符处理，遇到普通字符直接输出，遇到 `%` 开始解析格式说明符。

```cpp
template <typename OutputFn>
void vkprintf_impl(OutputFn&& putc_fn, const char* fmt, va_list args) {
    char buffer[64];

    while (*fmt != '\0') {
        if (*fmt != '%') {
            putc_fn(*fmt++);
            continue;
        }
        fmt++;  // consume '%'

        bool zero_pad = false;
        int  width    = 0;
        if (*fmt == '0') { zero_pad = true; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        char type = *fmt++;
        // ... switch on type ...
    }
}
```

格式说明符的解析分三步：先检查是否有 `0` 标志（零填充），然后读取宽度数字（比如 `%08x` 中的 `08` 表示宽度为 8、零填充），最后读取类型字符。宽度解析用了一个简单的 `while` 循环把多位数字拼起来——`width = width * 10 + (*fmt - '0')`。接下来根据类型字符做 switch-case 分发。

`%d` 和 `%u` 的处理方式很相似：通过 `va_arg` 取出参数，传给 `format_decimal`，然后跳转到统一的填充逻辑 `do_padding`。区别只在于 `va_arg` 取出的类型——`%d` 取 `int`，`%u` 取 `unsigned int`，然后都 `static_cast` 为 `int64_t`。这里有一个值得注意的设计决策：我们不支持长度修饰符（`%ld`、`%lld`），`%d` 直接按 `int` 取参数，`%x` 直接按 `uint64_t` 取参数。在我们的 64 位内核中，所有地址和值都是 64 位的，统一用 `uint64_t` 省去了长度修饰符的解析逻辑。

`%x` 和 `%X` 类似，区别只在于传给 `format_hex` 的 `lowercase` 参数。`va_arg` 取出的是 `uint64_t`，这意味着 `%x` 可以直接打印 64 位值。

`%p` 是最特殊的格式说明符，它不走 `do_padding` 路径，而是有自己的输出逻辑：

```cpp
case 'p':
    putc_fn('0');
    putc_fn('x');
    len = format_hex(va_arg(args, uint64_t), buffer, sizeof(buffer), false);
    for (int i = len; i < 16; i++) {
        putc_fn('0');
    }
    for (int i = 0; i < len; i++) {
        putc_fn(buffer[i]);
    }
    break;
```

先输出 `"0x"` 前缀，然后调用 `format_hex` 得到十六进制字符串，如果长度不足 16 位就在前面补零。这意味着 `%p` 的输出永远是 `0x` 加 16 个十六进制数字——比如 `0x0000000000200000`。为什么固定 16 位？因为在 x86_64 上，虚拟地址理论上是 64 位宽（实际使用 48 或 57 位），16 个十六进制数字正好对应 64 位，这样每个指针输出的宽度一致，在日志中对齐排列时非常直观。当你在调试页表时看到 `0x0000000000201003`，就能立刻识别出 bit 12-51 是物理页帧号、低 12 位是权限标志位。

`do_padding` 标签处的填充逻辑处理两种填充方式：如果 `zero_pad` 为 true，填充字符是 `'0'`，否则是空格。填充只在格式化出来的实际字符数小于要求的最小宽度时生效，填充字符全部在数字前面输出（左填充）。

`%s` 通过 `va_arg(args, const char*)` 取出字符串指针，如果为 `nullptr` 就用 `"(null)"` 代替——这个 `(null)` 的行为是 glibc printf 的惯例，Linux 内核的 printk 也是这么做的。`%c` 取出 `int` 类型（这是 C 标准的要求——`char` 类型的可变参数会被提升为 `int`），转为字符后输出。`%%` 直接输出一个百分号。对于未知格式说明符（比如 `%q`），default 分支原样输出 `%` 加上那个字符——这比静默吞掉好，至少你在终端上能看到 `%q`，知道格式字符串里有个笔误。

## 第五步——公共接口：包装函数与 g_serial 单例

格式化引擎之上是三个公共接口函数，它们都围绕一个文件局部的 `g_serial` 单例来工作。`g_serial` 是一个 `Serial` 对象，用 `SERIAL_COM1`（0x3F8）构造，放在匿名命名空间里作为文件局部变量——这是一个非常轻量的单例模式，不需要懒初始化、不需要线程安全，就是一个简单的文件局部全局变量。

```cpp
namespace {
using cinux::drivers::Serial;
using cinux::drivers::SERIAL_COM1;
static Serial g_serial(SERIAL_COM1);
}  // anonymous namespace
```

`kprintf_init()` 只做一件事——调用 `g_serial.init()`，配置 UART 寄存器为 115200 8N1 轮询模式。

`kprintf()` 的实现是一个标准的可变参数包装：

```cpp
void kprintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vkprintf_impl([&](char c) { g_serial.putc(c); }, fmt, args);
    va_end(args);
}
```

`va_start` 初始化 `va_list`，然后调用 `vkprintf_impl`，传入一个 lambda 作为 `OutputFn`。这个 lambda 捕获了 `g_serial` 的引用，每次被调用时往串口写一个字符。`va_end` 清理 `va_list` 资源。

`kvprintf()` 和 `kprintf()` 几乎一样，区别在于它直接接受 `va_list` 参数——适用于需要把 `va_list` 透传的场景。

`kpanic()` 是最戏剧性的函数：

```cpp
[[noreturn]] void kpanic(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vkprintf_impl([&](char c) { g_serial.putc(c); }, fmt, args);
    va_end(args);

    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

它先输出格式化消息，然后进入无限循环——`cli` 关中断，`hlt` 让 CPU 进入低功耗状态。因为已经 `cli` 了，CPU 永远不会被可屏蔽中断唤醒。`while(1)` 循环是必要的——某些 CPU 微架构在没有中断的情况下可能会从 `hlt` 中意外唤醒（比如 SMI 或者 NMI），循环确保即使被唤醒也会立刻再次 `cli; hlt`。`[[noreturn]]` 属性告诉编译器这个函数永远不会返回，允许编译器做更激进的优化（比如不生成函数调用之后的清理代码）。

## 第六步——单元测试：Mock 输出的巧妙设计

kprintf 的单元测试面临一个根本性的挑战：我们无法在 host 端（你的开发机上）直接测试内核的格式化逻辑，因为它依赖串口 I/O 端口操作，而你的开发机上没有也不应该有直接操作 I/O 端口的权限。解决方案正是 `vkprintf_impl` 的模板设计带来的好处——测试代码不链接真正的串口驱动，而是提供一个 mock 输出函数，把格式化结果追加到一个 `std::string` 缓冲区里，然后比对缓冲区内容和预期输出。

测试文件 `test_kprintf.cpp` 被 `#ifdef CINUX_HOST_TEST` 包裹，只在 host 端编译时启用。它定义了一个 `MockFormatter` 类：

```cpp
class MockFormatter {
public:
    void putc(char c) { buffer_.push_back(c); }

    void puts(const char* s) {
        while (*s) {
            if (*s == '\n') putc('\r');
            putc(*s++);
        }
    }

    std::string result() const { return buffer_; }
    void        clear() { buffer_.clear(); }

private:
    std::string buffer_;
};
```

`putc` 把字符追加到内部的 `std::string` 缓冲区，`puts` 额外处理了 `\n` 到 `\r\n` 的转换，`result()` 返回缓冲区内容用于断言。

由于 `vkprintf_impl` 被放在匿名命名空间里，翻译单元外不可见，测试代码无法直接调用它。所以测试文件中镜像实现了格式化辅助函数（`format_decimal` 和 `format_hex` 直接复制过来——它们是纯算术运算没有外部依赖）和格式解析逻辑（`mock_vprintf` 函数复刻了 `vkprintf_impl` 的 switch-case 分发逻辑）。这是一个有意的取舍：我们测试的是格式化算法的正确性，而不是某个具体函数的行为。

测试用例覆盖了以下场景，我来挑几个值得说的展开一下。

首先是十进制格式化的基本场景——正数、负数、零值、INT64_MAX：

```cpp
TEST("kprintf: format positive decimal") {
    char buf[64];
    int  len = format_decimal(42, buf, sizeof(buf));
    ASSERT_EQ(len, 2);
    ASSERT_EQ(std::string(buf), "42");
}

TEST("kprintf: format INT64_MAX") {
    char buf[64];
    int  len = format_decimal(9223372036854775807LL, buf, sizeof(buf));
    ASSERT_EQ(len, 19);
    ASSERT_EQ(std::string(buf), "9223372036854775807");
}
```

INT64_MAX 的测试特别重要——它验证了 19 位十进制数字的格式化是正确的，临时数组不会溢出。

然后是十六进制格式化，包括大小写和零值：

```cpp
TEST("kprintf: format lowercase hex") {
    char buf[64];
    int  len = format_hex(0xDEADBEEF, buf, sizeof(buf), true);
    ASSERT_EQ(len, 8);
    ASSERT_EQ(std::string(buf), "deadbeef");
}
```

`0xDEADBEEF` 是一个经典的测试用例——它包含 A-F 的所有十六进制字母，能同时验证数字字符和字母字符的转换。

边界条件测试验证了缓冲区太小时的截断行为和零大小缓冲区的安全性：

```cpp
TEST("kprintf: format decimal with small buffer") {
    char buf[3];
    int  len = format_decimal(12345, buf, sizeof(buf));
    ASSERT_TRUE(len <= 2);
}
```

这个测试确认了即使缓冲区只有 3 字节，`format_decimal` 也不会越界写入——最多写入 `buffer_size - 1` 个字符加一个 `\0`。

`%p` 指针格式的测试验证了 16 位十六进制的固定宽度输出：

```cpp
TEST("kprintf: %p with zero address") {
    char buf[64];
    int  len = format_hex(0, buf, sizeof(buf), false);

    std::string result = "0x";
    for (int i = len; i < 16; i++) result += '0';
    result += std::string(buf);

    ASSERT_EQ(result, "0x0000000000000000");
}
```

零地址应该输出 `0x0000000000000000`——16 个零加上 `0x` 前缀。

完整的格式化引擎测试通过 `mock_vprintf` 覆盖了各种格式符的组合——`%d` 正数负数零值、`%u` 大数、`%x`/`%X` 大小写、`%s` 正常字符串和 `nullptr`、`%c` 字符、`%%` 百分号转义、`%p` 指针、`%08x` 零填充、`%4d` 空格填充、混合格式，以及未知格式说明符的原样输出。总共 30 多个测试用例，覆盖了正常路径、边界条件和异常输入。

## 上板验证

单元测试跑通了，现在我们把代码烧到 QEMU 里看看真实效果。构建并运行：

```bash
cd build && cmake --build . -j$(nproc) && make run
```

kprintf 在内核启动的最早期就被初始化了——`kprintf_init()` 在 `mini_kernel_main` 的开头被调用，之后整个内核的所有输出都通过 kprintf 完成。串口输出应该是这样的（截取关键部分）：

```
Cinux Mini Kernel v0.1.0
[INIT] Setting up GDT...
[INIT] GDT loaded successfully.
[INIT] Setting up IDT...
[INIT] IDT loaded successfully.
[INIT] Initializing ATA controller...
[INIT] ATA controller initialized successfully (status=0x50).
[DEMO] MBR boot signature: 0xaa55 (VALID)
```

你会注意到 `%p` 格式的输出像 `0x0000000000200000` 这样的完整 16 位十六进制——在日志中排列非常整齐，一目了然就是指针地址。`%x` 输出像 `0xaa55` 这样不带 `0x` 前缀（`0x` 是格式字符串里手写的），和 `%p` 明确区分。负数输出像 `-12345` 带负号前缀，零值输出单个 `0`。

如果你触发一个 `kpanic`，输出会类似：

```
[PANIC] Assertion failed: pmm_alloc_page() returned null
[PANIC] at kernel/mm/pmm.cpp:142
```

然后 QEMU 会停在那里不动——`cli; hlt` 死循环生效了。

单元测试的运行结果：

```bash
cd build && make test_host
```

输出应该是全部 PASS：

```
[PASS] kprintf: format positive decimal
[PASS] kprintf: format negative decimal
[PASS] kprintf: format zero
[PASS] kprintf: format INT64_MAX
[PASS] kprintf: format lowercase hex
[PASS] kprintf: format uppercase hex
[PASS] kprintf: format hex zero
[PASS] kprintf: format hex single digit
[PASS] kprintf: format decimal with small buffer
[PASS] kprintf: format hex with zero buffer
[PASS] kprintf: %p produces 0x prefix + 16 hex digits
[PASS] kprintf: %p with zero address
[PASS] kprintf: mock serial captures output
[PASS] kprintf: mock serial converts LF to CRLF
[PASS] kprintf engine: %d positive
[PASS] kprintf engine: %d negative
[PASS] kprintf engine: %d zero
[PASS] kprintf engine: %u large
[PASS] kprintf engine: %x lowercase
[PASS] kprintf engine: %X uppercase
[PASS] kprintf engine: %s normal
[PASS] kprintf engine: %s nullptr
[PASS] kprintf engine: %s empty
[PASS] kprintf engine: %c character
[PASS] kprintf engine: %% literal percent
[PASS] kprintf engine: %p pointer
[PASS] kprintf engine: %p zero
[PASS] kprintf engine: %08x zero-pad
[PASS] kprintf engine: %4d space-pad
[PASS] kprintf engine: mixed format
[PASS] kprintf engine: unknown specifier fallback
```

30 多个测试用例全部通过，覆盖了所有格式说明符的正常路径和边界条件。

## 踩坑总结

写 kprintf 的过程中踩了几个值得记录的坑，按"痛的程度"排列。

最痛的坑就是 INT64_MIN 的补码陷阱。第一次写的时候完全没想到这个情况——对 `INT64_MIN` 取负在补码下结果还是 `INT64_MIN`，因为绝对值超出了 `int64_t` 的表示范围。测试的时候打印了几个负数感觉没问题就过了，直到后来做边界测试的时候才暴露出来。正确的处理方式是在取负之前做特判，直接硬编码输出 `"-9223372036854775808"`。更优雅的替代方案是用无符号算术：`abs_val = static_cast<uint64_t>(-(value + 1)) + 1`——先加 1 再取负再加回来，避免了对 `INT64_MIN` 的直接取负。但我们选择了硬编码方案，因为它让 INT64_MIN 的特殊性一目了然，而且这种情况在实际运行中极其罕见。

第二个坑是 `do { ... } while` 和 `while { ... }` 的选择。格式化数字的时候，如果值是 0，`while (abs_val > 0)` 循环一次都不执行，结果就是空字符串——输出什么都没有而不是 `"0"`。用 `do-while` 可以保证至少执行一次循环体，对 0 这个特殊值产生正确的输出。这是一个非常基础的编程错误，但恰恰因为基础，反而容易在第一遍写的时候忽略。

第三个坑是宽度解析中 `0` 标志和数字的冲突。格式字符串 `%08x` 中，第一个 `0` 是零填充标志，后面的 `8` 是宽度值。如果解析的时候不先检查 `0` 标志就直接进宽度解析，会把 `0` 当成宽度值的一部分（`0 * 10 + 0 = 0`），零填充就失效了。我们的做法是先检查 `*fmt == '0'`，设置 `zero_pad = true` 并跳过这个字符，然后再进宽度数字的循环。

第四个坑是测试代码不能直接调用 `vkprintf_impl`。因为它被放在匿名命名空间里，翻译单元外不可见。解决方案是在测试文件中镜像实现一个 `mock_vprintf`，复刻格式解析逻辑。格式化辅助函数可以直接复制过来测试（纯算术运算没有外部依赖），格式解析逻辑通过镜像实现覆盖。这么做的原因是把 `vkprintf_impl` 放在匿名命名空间是有意为之——它是一个内部实现细节，不应该暴露给外部代码。代价是测试需要维护一份镜像，如果格式解析逻辑改了，测试代码也要同步改。

## 收尾

到这里，kprintf 格式化输出引擎就全部完成了。从 `va_list` 的底层机制，到除法取余和位掩码的数字格式化算法，到模板驱动的格式解析引擎，再到 Mock 输出的单元测试策略——每一步都有其设计考量和值得注意的细节。整个引擎不到 300 行代码，但它让我们的内核从此拥有了"说话"的能力，后续所有的调试信息、错误报告、panic 输出都会通过 kprintf 来完成。

从架构上看，kprintf 处于"驱动层之上、业务逻辑之下"的位置——它依赖串口驱动来输出字符，而上层的 GDT 初始化、内存管理、中断处理等模块都依赖它来输出调试信息。`OutputFn` 模板参数的设计让格式化引擎和输出后端完全解耦，未来如果需要支持 VGA 文本模式、framebuffer、或者网络日志输出，只需要提供一个新的 lambda 就行，格式化逻辑不需要改一行。

和 xv6 的 printf 相比，我们的 kprintf 有宽度修饰符和 `%p` 支持，格式化能力更强。和 Linux 内核的 printk 相比，我们缺少日志级别、ring buffer 和时间戳，但这些功能会在后续章节逐步加上。目前这个 kprintf 的功能已经足够支撑内核的开发调试了。

下一步，我们要解决大内核 ELF 加载器中的一个关键 bug——这就是 009D 的内容了。具体来说，我们发现之前实现的 ELF 加载器在处理某些特殊的段布局时会出问题，需要修复才能让后续的大内核加载测试顺利通过。

---

> 本章对应 milestone：`009_big_kernel_kprintf`
> 上一章：[009B - 大内核串口驱动](009B-big-kernel-serial.md)
> 下一章预告：009D - 大内核 ELF 加载器 Bug 修复
