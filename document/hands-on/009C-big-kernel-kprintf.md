# 009C Big Kernel 格式化输出引擎 —— 在 freestanding 环境下手搓 kprintf

## 章节导语

上一章（009B）我们把串口驱动搞定了，内核终于有了一条能和外界通信的管道——往 COM1 端口写字节，终端上就能看到。但说实话，每次输出都要手动把数字拆成字符一个一个地塞进串口，这种代码写多了人会疯。我们需要一个 printf 风格的格式化引擎，能写 `kprintf("count = %d, addr = %p\n", 42, 0x1000000)` 这种一行代码就把活干了的东西。

问题是，我们的内核是 freestanding 环境——编译时带了 `-ffreestanding -nostdlib`，没有标准库，自然也没有 `printf`。链接器碰到 `printf` 这个符号只会冷冰冰地报 `undefined reference`。所以这条路只有一个选择：自己写一个。

完成本章后，我们的大内核就拥有了一个完整的格式化输出引擎 `kprintf`，支持 `%d`、`%u`、`%x`、`%X`、`%p`、`%s`、`%c`、`%%` 等常用格式符，以及宽度修饰符（`%08x`、`%4d` 等）。同时我们还会在 host 端写一套完整的单元测试来验证每个格式符的正确性——毕竟格式化函数的边界情况多到让人怀疑人生。

本章的前置知识是 009B 的串口驱动。我们需要理解 `Serial::putc` 是怎么往 COM1 端口写一个字节的，因为 `kprintf` 的底层输出就是通过它实现的。

---

## 环境说明

这个章节的开发环境和 009B 完全一致，不涉及任何硬件变更。我们在 QEMU 模拟的 x86_64 环境下工作，串口输出通过 `-serial stdio` 参数直接打印到终端。工具链方面，GCC/G++ 配合 CMake 构建，编译选项包含 `-ffreestanding -nostdlib -fno-exceptions -fno-rtti`，意味着没有标准库、没有异常、没有 RTTI。我们唯一能依赖的"标准"头文件只有 `<stdarg.h>`、`<stdint.h>`、`<stddef.h>` 这几个编译器内置的 freestanding 头文件。

本章涉及的源文件如下：
- `kernel/lib/kprintf.hpp` —— kprintf 公共接口声明
- `kernel/lib/kprintf.cpp` —— 格式化引擎完整实现
- `tests/unit/test_kprintf.cpp` —— host 端单元测试（445 行）
- `kernel/drivers/serial.hpp` —— 串口驱动（上一章完成，本章作为输出后端使用）
- `kernel/main.cpp` —— 大内核入口，展示 kprintf 使用方式

---

## 第一步——为什么没有 printf，以及 va_list 到底是怎么回事

在正式动手之前，我们有必要搞清楚两件事：为什么不能用 printf，以及我们要用到的 `<stdarg.h>` 到底是什么。

### freestanding 环境下 printf 的缺席

我们的大内核编译时带了 `-ffreestanding`。这个标志告诉编译器："我不依赖任何标准库实现，你只提供 freestanding 环境必需的头文件就行了。" 在 C/C++ 规范里，freestanding 环境只需要提供极少数头文件：`<stddef.h>`、`<stdint.h>`、`<stdarg.h>`、`<stdbool.h>`、`<limits.h>`、`<float.h>` 等寥寥几个。`<stdio.h>` 和里面的 `printf` 不在其中。

再加上链接时带了 `-nostdlib`，链接器不会自动链接 libc.so 或 libc.a。即使你强行 `#include <stdio.h>`（GCC 可能会找到这个头文件），到链接阶段也会收获一堆 `undefined reference to printf` 的报错。所以与其折腾怎么把标准库塞进内核，不如花点时间写一个够用的格式化引擎——毕竟我们也不需要浮点数格式化、域宽对齐这些花里胡哨的功能。

### va_list：编译器替我们做的黑魔法

`<stdarg.h>` 是 freestanding 头文件之一，这意味着即使在 `-ffreestanding` 环境下我们也可以用它。它提供了三个宏和一个类型：

- `va_list`：一个不透明类型，代表可变参数列表的迭代器
- `va_start(va_list, last_named_param)`：初始化 va_list，让它指向第一个可变参数
- `va_arg(va_list, type)`：从 va_list 中取出一个类型为 `type` 的参数，同时把迭代器推进一步
- `va_end(va_list)`：清理 va_list

这几个宏的实现是编译器内置的（在 GCC 里是 `__builtin_va_start` 等），不依赖任何标准库。它们的底层机制和 CPU 调用约定紧密相关。在 x86-64 System V ABI 下，函数调用的前 6 个整数/指针参数通过寄存器传递（依次是 rdi、rsi、rdx、rcx、r8、r9），浮点参数走 xmm 寄存器，超出的参数才压栈。`va_list` 内部需要记录"哪些寄存器还没被消费完"和"栈上参数的当前偏移"这两部分信息——所以 x86-64 的 `va_list` 实际上是一个结构体而不是简单的指针，比 32 位时代的实现复杂得多。

不过这些底层细节对我们来说是透明的。我们只需要知道一件事：`va_arg(args, type)` 会从参数列表中取出一个 `type` 类型的值。它不会帮你检查类型是否匹配——如果你写 `va_arg(args, int)` 但实际传进来的是一个 `char*`，编译器不会报错，运行时只会给你一个垃圾值或者直接段错误。这也是为什么 printf 的格式字符串和参数类型必须严格对应——C 语言把类型安全的责任全甩给了程序员。

---

## 第二步——格式化引擎架构设计：泛型 OutputFn 模板

现在我们开始看真正的代码。首先要解决一个架构层面的问题：格式化引擎的输出目标是什么？

如果只考虑串口输出，最直白的做法是让格式化函数直接调用 `Serial::putc`。但这样做有个问题——测试的时候怎么办？我们的 host 端单元测试跑在 Linux 上，没有 COM1 端口，调用 `Serial::putc` 会直接触发 I/O 端口访问然后段错误。所以我们希望格式化引擎和输出目标解耦：同一个格式化逻辑，既能往串口写字节，也能往测试缓冲区写字节。

C++ 给我们提供了一个非常优雅的解决方案——模板。我们来看 `vkprintf_impl` 的签名：

```cpp
// kernel/lib/kprintf.cpp (anonymous namespace)

template <typename OutputFn>
void vkprintf_impl(OutputFn&& putc_fn, const char* fmt, va_list args) {
    // ... 格式化逻辑，对每个输出字符调用 putc_fn(c)
}
```

`OutputFn` 是一个模板类型参数，可以是任何可调用对象（函数指针、lambda、仿函数），只要接受一个 `char` 参数就行。`OutputFn&&` 是万能引用（forwarding reference），既能接受左值也能接受右值。使用模板而不是函数指针有一个重要的好处：编译器可以在编译时内联整个调用——如果 `putc_fn` 是一个 lambda，编译器能看到 lambda 的完整实现，直接把 lambda 体嵌入格式化循环里，消除函数调用的开销。这就是 C++ 常说的"零开销抽象"——抽象了输出目标，但运行时性能和不抽象一样好。

在生产代码里，我们这样调用它：

```cpp
// kernel/lib/kprintf.cpp

void kprintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vkprintf_impl([&](char c) { g_serial.putc(c); }, fmt, args);
    va_end(args);
}
```

这里传入的 `OutputFn` 是一个捕获了 `g_serial` 的 lambda：`[&](char c) { g_serial.putc(c); }`。在测试代码里，我们传入的则是另一个 lambda，它把字符追加到一个 `std::string` 缓冲区——同一个格式化引擎，两种输出目标，完全不需要修改格式化逻辑本身。

---

## 第三步——数字格式化：十进制与十六进制

格式化引擎最核心的部分是数字转字符串。我们实现了两个文件局部的辅助函数：`format_decimal` 处理有符号十进制，`format_hex` 处理无符号十六进制。它们都被放在匿名命名空间里，对外不可见——这是 C++ 实现"文件私有函数"的标准做法。

### 十进制格式化：除法取余法

把一个整数转成十进制字符串的经典算法是"除法取余法"——反复除以 10 取余数，得到的是从最低位到最高位的数字。因为我们是先得到低位再得到高位，所以需要先存到一个临时缓冲区里，然后逆序拷贝到输出缓冲区。

```cpp
// kernel/lib/kprintf.cpp

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

这段代码有几个值得细说的地方。

先说 INT64_MIN 的特殊处理。你可能觉得奇怪：为什么要单独判断 `value == (-9223372036854775807LL - 1)` 而不是直接 `value == INT64_MIN`？原因是 `INT64_MIN` 这个宏可能在某些环境下不可用（我们毕竟是 freestanding），而且 `(-9223372036854775807LL - 1)` 这个写法更明确地表达了它的值。但更关键的问题是：**为什么需要特殊处理？** 因为 `INT64_MIN = -2^63 = -9223372036854775808`，它的绝对值 `2^63 = 9223372036854775808` 超出了 `int64_t` 的表示范围（最大正数是 `2^63 - 1`）。如果你直接写 `value = -value`，对于 INT64_MIN 来说结果是未定义行为——在 x86-64 上用 `neg` 指令执行的话，溢出后得到的还是 INT64_MIN 本身（负数取负还是负数，非常反直觉）。所以我们对这个值做硬编码字符串输出，直接把 `"-9223372036854775808"` 拷贝进缓冲区。

说实话，这个坑我第一次写的时候完全没想到，是跑单元测试的时候发现 INT64_MAX 能过但 INT64_MIN 输出是乱码才反应过来的。这种"二进制补码的不对称性"问题在各种底层代码里反复出现，每次都能坑到一批人。

然后看主循环。`do { ... } while` 而不是 `while { ... }` 是有讲究的：当 `value == 0` 的时候，我们仍然需要输出一个 `'0'` 字符，所以至少要执行一次循环体。`tmp[24]` 足够容纳 `int64_t` 的最大十进制位数（19 位）加一个符号位。逆序拷贝的逻辑是 `tmp[--tmp_idx]`——因为 `tmp_idx` 在循环结束时指向"最后一个写入位置的下一个位置"，所以要先减一再取。

### 十六进制格式化：位运算取数

十六进制比十进制简单得多，因为每 4 个二进制位恰好对应一个十六进制位。不需要做除法，只要做位与（`& 0xF`）取最低 4 位然后右移 4 位就行了。

```cpp
// kernel/lib/kprintf.cpp

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

`digits` 数组相当于一个查找表——`digits[0]` 到 `digits[9]` 是 `'0'` 到 `'9'`，`digits[10]` 到 `digits[15]` 是 `'a'` 到 `'f'`（或者大写版本）。`value & 0xF` 取出最低 4 位（0-15），直接用这个值作为数组下标就能得到对应的字符。和十进制一样，结果也是逆序的，需要翻转过来。`tmp[20]` 足够容纳 `uint64_t` 的最大十六进制位数（16 位）。

`lowercase` 参数控制输出大小写：`%x` 对应小写 `abcdef`，`%X` 对应大写 `ABCDEF`。这个参数由 `vkprintf_impl` 里的格式解析传入。

---

## 第四步——vkprintf_impl：格式解析主循环

现在我们来看格式化引擎的主体——`vkprintf_impl`。这个函数接受一个格式字符串和一个 va_list，逐字符扫描格式字符串，遇到普通字符直接输出，遇到 `%` 开头的格式符就解析并格式化对应的参数。

```cpp
// kernel/lib/kprintf.cpp (anonymous namespace)

template <typename OutputFn>
void vkprintf_impl(OutputFn&& putc_fn, const char* fmt, va_list args) {
    char buffer[64];

    while (*fmt != '\0') {
        if (*fmt != '%') {
            putc_fn(*fmt++);
            continue;
        }

        // Consume '%'
        fmt++;

        // Parse optional zero-pad flag
        bool zero_pad = false;
        int  width    = 0;

        if (*fmt == '0') {
            zero_pad = true;
            fmt++;
        }

        // Parse optional width
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        char type = *fmt++;
        int  len  = 0;

        switch (type) {
        case '%':
            putc_fn('%');
            break;

        case 'c':
            putc_fn(static_cast<char>(va_arg(args, int)));
            break;

        case 's': {
            const char* s = va_arg(args, const char*);
            if (s == nullptr) s = "(null)";
            while (*s) putc_fn(*s++);
            break;
        }

        case 'd':
            len = format_decimal(static_cast<int64_t>(va_arg(args, int)),
                                 buffer, sizeof(buffer));
            goto do_padding;

        case 'u':
            len = format_decimal(static_cast<int64_t>(va_arg(args, unsigned int)),
                                 buffer, sizeof(buffer));
            goto do_padding;

        case 'x':
            len = format_hex(va_arg(args, uint64_t), buffer, sizeof(buffer), true);
            goto do_padding;

        case 'X':
            len = format_hex(va_arg(args, uint64_t), buffer, sizeof(buffer), false);
            goto do_padding;

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

        do_padding:
            if (len < width) {
                char pad = zero_pad ? '0' : ' ';
                for (int i = width - len; i > 0; i--) {
                    putc_fn(pad);
                }
            }
            for (int i = 0; i < len; i++) {
                putc_fn(buffer[i]);
            }
            break;

        default:
            putc_fn('%');
            putc_fn(type);
            break;
        }
    }
}
```

这段代码的信息量不小，我们逐块拆解。

首先是主循环的结构。`while (*fmt != '\0')` 逐字符扫描格式字符串。遇到非 `%` 的字符直接输出并前进；遇到 `%` 则进入格式符解析逻辑。解析过程分三步：先看有没有 `'0'` 标志（零填充），再看有没有宽度数字（支持多位数如 `%10d`），最后看类型字符。

接下来看各个格式符的处理。`%%` 最简单，直接输出一个 `%` 字面量。`%c` 从 va_list 取一个 `int` 然后转成 `char` 输出——注意这里 `va_arg(args, int)` 而不是 `va_arg(args, char)`，因为 C/C++ 的可变参数会把 `char` 提升为 `int`，直接用 `va_arg(args, char)` 在某些平台上会有对齐问题。`%s` 取一个 `const char*` 指针然后逐字符输出，这里有个 nullptr 安全处理：如果传入的是空指针，输出 `"(null)"` 而不是直接段错误。这个处理看似微不足道，但在内核调试的时候特别有用——你不会想因为某个字符串参数是 NULL 就导致整个内核 crash 吧。

`%d` 和 `%u` 的处理逻辑基本一样：调用 `format_decimal` 把数字转成字符串存到 `buffer` 里，然后跳到 `do_padding` 标签做宽度填充。区别在于 `va_arg` 的类型：`%d` 取 `int`，`%u` 取 `unsigned int`。`%x` 和 `%X` 调用 `format_hex`，传给它的 `lowercase` 参数分别是 `true` 和 `false`。

这里用了一个 `goto do_padding` 的技巧。`%d`、`%u`、`%x`、`%X` 这四种格式符都需要做宽度填充，如果为每种格式符都写一遍填充逻辑，代码会非常冗余。把它们统一跳到 `do_padding` 标签处理是一种简洁的做法——虽然在结构化编程的教科书里 `goto` 是"邪恶的"，但在这种 switch-case 内部的局部跳转场景下，它比复制粘贴四段代码要干净得多。

`do_padding` 的逻辑很直白：如果格式化后的字符串长度 `len` 小于指定宽度 `width`，就用填充字符（零或空格）补齐到 `width`。注意填充是**左填充**——先输出填充字符，再输出实际内容。这意味着 `%4d` 输出 `42` 的结果是 `"  42"`（前面两个空格），而 `%04d` 输出 `42` 的结果是 `"0042"`（前面两个零）。

`%p` 的处理比较特殊：它固定输出 `"0x"` 前缀加上 16 位十六进制数，不足 16 位用前导零补齐。这意味着 `%p` 输出的永远是 18 个字符（`"0x"` + 16 位十六进制），格式统一，方便在日志里对齐查看地址。比如地址 `0x1000000` 输出为 `"0x0000000001000000"`。

最后是 `default` 分支：遇到不认识的格式符（比如 `%q`），原样输出 `%` 加上那个字符。这样至少不会丢信息，调试的时候也能看出"这里有个我不支持的格式符"。

---

## 第五步——公共接口封装：kprintf / kvprintf / kpanic

格式化引擎实现好了，接下来我们把它包装成三个公共接口。这三个函数被放在 `cinux::lib` 命名空间下，声明在 `kernel/lib/kprintf.hpp` 里。

### 全局串口实例

```cpp
// kernel/lib/kprintf.cpp (anonymous namespace)

static Serial g_serial(SERIAL_COM1);
```

匿名命名空间里有一个全局的 `Serial` 对象 `g_serial`，绑定到 COM1 端口。它是文件私有的——外部代码不能直接访问它，只能通过 `kprintf_init()` 初始化、通过 `kprintf()` 系列函数间接使用它。

### kprintf_init：一次性初始化

```cpp
// kernel/lib/kprintf.cpp

namespace cinux::lib {

void kprintf_init() {
    g_serial.init();
}
```

`kprintf_init()` 在大内核启动时被调用一次（在 `kernel_main()` 里），负责把 COM1 端口配置为 115200 8N1 模式。如果忘了调用这个函数就直接用 `kprintf`，串口端口处于未初始化状态，数据发不出去——QEMU 那边什么都收不到，你会对着一个毫无输出的终端怀疑人生。所以这个初始化一定要放在内核入口的最前面。

### kprintf：可变参数版本

```cpp
void kprintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vkprintf_impl([&](char c) { g_serial.putc(c); }, fmt, args);
    va_end(args);
}
```

这是我们最常调用的接口。`...` 表示可变参数，`va_start(args, fmt)` 让 `args` 指向 `fmt` 之后的第一个可变参数。然后我们把一个捕获了 `g_serial` 的 lambda 作为 `OutputFn` 传给 `vkprintf_impl`。这个 lambda 做的事情很简单：对每个要输出的字符调用 `g_serial.putc(c)`。`va_end(args)` 清理 va_list 资源——虽然在我们当前的实现里不调用 `va_end` 可能也不会出问题，但规范要求每个 `va_start` 必须配一个 `va_end`，不遵守在某些平台上会导致栈损坏。

### kvprintf：va_list 版本

```cpp
void kvprintf(const char* fmt, va_list args) {
    vkprintf_impl([&](char c) { g_serial.putc(c); }, fmt, args);
}
```

`kvprintf` 接受一个已经初始化好的 `va_list` 而不是 `...`。这种设计模式在标准库里也很常见（`printf` 对应 `vprintf`）。它的用途是当你想写一个自己的可变参数函数，内部调用 `kprintf` 的格式化能力时，可以通过 `kvprintf` 把 va_list 传递进去，而不需要做 va_list 的"解包再重包"操作。

### kpanic：打印然后永久停机

```cpp
[[noreturn]] void kpanic(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vkprintf_impl([&](char c) { g_serial.putc(c); }, fmt, args);
    va_end(args);

    // Halt forever
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

`kpanic` 是内核的"最后遗言"——打印一条错误信息然后永久停机。`[[noreturn]]` 属性告诉编译器这个函数永远不会返回，编译器可以据此做一些优化（比如不需要为调用者保存返回地址后的寄存器状态）。死循环里用了 `cli; hlt` 两条指令：`cli` 禁用中断，`hlt` 让 CPU 进入低功耗停机状态。如果只写 `hlt` 不写 `cli`，外部硬件中断可能会把 CPU 从 halt 状态唤醒，然后继续执行循环——加了 `cli` 之后，除非发生 NMI（不可屏蔽中断），CPU 会永远停在这里。

---

## 第六步——单元测试：Mock 设计与测试用例讲解

格式化函数是那种"看起来简单但边界情况特别多"的典型。正数、负数、零、最大值、最小值、空指针、空字符串、各种宽度组合……如果每个情况都靠启动 QEMU 肉眼看串口输出来验证，效率低到让人崩溃。所以我们把格式化逻辑在 host 端做单元测试——用 `CINUX_HOST_TEST` 宏来 mock 掉硬件依赖。

### CINUX_HOST_TEST 宏的作用

`CINUX_HOST_TEST` 是我们在测试框架里定义的一个宏，当它被定义时，测试代码会走 host 端的路径：用 `printf` 代替串口输出、用 `abort()` 代替内核 halt、用标准库的 `std::string` 做缓冲区。这样测试文件可以用普通的 g++ 编译并在 Linux 上运行，不需要 QEMU。

### Mock 设计思路

我们不能直接 `#include "kernel/lib/kprintf.cpp"` 然后在 host 端运行，因为那个文件依赖 `Serial` 类和 I/O 端口操作——在 host 上这些都会段错误。所以我们的测试策略是：

1. 把 `format_decimal` 和 `format_hex` 两个纯算术函数**复制一份**到测试文件里（它们没有任何硬件依赖，只是整数运算）
2. 在测试文件里**重写一份 `vkprintf_impl` 的解析逻辑**（叫做 `mock_vprintf`），但是把 `OutputFn` 换成一个把字符追加到 `std::string` 的 MockFormatter 类
3. 这样整个格式化链条就是自包含的，可以在 host 上跑

```cpp
// tests/unit/test_kprintf.cpp

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

`MockFormatter` 就是一个简单的字符串收集器——每次 `putc` 追加一个字符到内部的 `std::string` 里，`result()` 返回完整的结果字符串用于断言。`puts` 方法额外处理了 `\n` 到 `\r\n` 的转换，模拟串口驱动的行为。

### 测试覆盖范围

我们的测试文件有 445 行，覆盖了以下所有场景：

**十进制格式化（format_decimal 直接测试）**：正数 `42`、负数 `-12345`、零 `0`、INT64_MAX `9223372036854775807`。INT64_MAX 这个测试很重要——19 位数字，刚好是 int64_t 十进制表示的最大位数。如果 `tmp[24]` 缓冲区不够大或者逆序拷贝的边界条件有 bug，这个测试就能抓出来。

**十六进制格式化（format_hex 直接测试）**：小写 `0xDEADBEEF` → `"deadbeef"`、大写 `0xDEADBEEF` → `"DEADBEEF"`、零 `0` → `"0"`、单位数 `0xA` → `"a"`。大小写测试确保 `lowercase` 参数正确传递，零和单位数测试确保 `do-while` 循环的"至少执行一次"语义是对的。

**缓冲区边界测试**：小缓冲区（3 字节格式化 5 位数，测试截断）、零大小缓冲区（测试不越界访问）。这些测试确保 `buffer_size` 参数的检查逻辑是有效的——在缓冲区不够的情况下，函数应该安全截断而不是越界写入。

**`%p` 格式测试**：正常地址 `0x1000000` 输出 `"0x0000000001000000"`（前导零补齐到 16 位）、零地址 `0x0` 输出 `"0x0000000000000000"`。这两个测试验证了 `%p` 的固定宽度语义。

**完整格式引擎测试（通过 mock_vprintf）**：这一组测试模拟了真实的 `kprintf` 调用，验证了各种格式符在完整格式字符串中的行为：
- `%d` 正数、负数、零
- `%u` 大数 `4000000000`
- `%x` 小写、`%X` 大写
- `%s` 正常字符串、nullptr（输出 `"(null)"`）、空字符串
- `%c` 多字符
- `%%` 字面量
- `%p` 指针
- `%08x` 零填充、`%4d` 空格填充
- 混合格式：`"%s=%d (0x%x)"` 输出 `"answer=42 (0x2a)"`
- 未知格式符 `%q` 输出 `"%q"`（原样保留）

其中混合格式测试特别有实战价值——它模拟了内核里最常见的 `kprintf` 使用场景，一次调用里混用多种格式符，验证了 va_list 的参数消费顺序是否正确。如果 `va_arg` 的类型和顺序搞错了，这个测试一定会暴露出来。

### 运行测试

```bash
cd build
make test_host
```

期望输出类似：

```
=== Cinux Test Runner ===
Running 22 test(s)...

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
...

=== Results: 22 passed, 0 failed ===
[SUITE PASSED]
```

22 个测试全绿，说明格式化引擎在 host 端的各种边界条件下都工作正常。但 host 端测试通过不代表 QEMU 里也没问题——我们还需要实际跑一下内核。

---

## 构建与运行

### 完整构建流程

```bash
# 从项目根目录
cmake -B build -DCMAKE_BUILD_TYPE=Debug -S .
cmake --build build -j$(nproc)
cd build
make run
```

### 期望的串口输出

```
[BIG] Big kernel running @ 0x1000000
```

这行输出来自 `kernel/main.cpp`：

```cpp
extern "C" void kernel_main() {
    cinux::lib::kprintf_init();
    cinux::lib::kprintf("[BIG] Big kernel running @ 0x1000000\n");
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

`kprintf_init()` 先初始化串口，然后 `kprintf` 输出那行里程碑信息。能看到这行输出就说明三件事都对了：串口初始化成功、格式化引擎正常工作、QEMU 串口转发正确。

如果你还想运行 host 端单元测试：

```bash
cd build
cmake -DCINUX_BUILD_TESTS=ON ..
cmake --build . -j$(nproc)
make test_host
```

---

## 调试技巧

### 串口没有任何输出

如果 `make run` 之后终端上一片空白，首先确认 `kprintf_init()` 是否被调用了。这个函数必须在使用 `kprintf` 之前调用，否则 `g_serial` 处于未初始化状态，往未配置的 I/O 端口写数据什么都发不出去。如果确认调用了但还是没输出，用 `make run-debug` 启动 GDB 调试，在 `kernel_main` 设断点，单步跟踪进入 `kprintf_init()` 和 `kprintf()`，看看是卡在串口初始化还是卡在格式化逻辑里。

### 数字格式化输出乱码

这种情况多半是 `format_decimal` 或 `format_hex` 的逆序拷贝逻辑有 bug。用 GDB 在 `vkprintf_impl` 里的 `format_decimal` 调用处设断点，查看 `buffer` 里的内容是否正确。常见错误包括：`tmp_idx` 的递减逻辑写反了（从 0 开始递增而不是从 `tmp_idx - 1` 开始递减）、缓冲区大小不够导致截断、`va_arg` 的类型不匹配导致取到的值本身就是错的。

### %p 输出长度不对

`%p` 应该固定输出 18 个字符（`"0x"` + 16 位十六进制）。如果输出长度不对，检查 `format_hex` 返回的 `len` 值——如果 `len > 16`，说明传入的值超过了 64 位（不太可能），或者 `format_hex` 有 bug。如果 `len < 16` 但前导零没有补齐，检查 `for (int i = len; i < 16; i++)` 这个循环是不是被意外跳过了。

### va_arg 类型不匹配的排查

这是最阴险的一类 bug。比如你在格式字符串里写了 `%d` 但传了一个 `uint64_t`，`va_arg(args, int)` 只会从参数列表中取 4 个字节而不是 8 个字节，后续的参数消费就全错位了——后面的 `%s` 可能取到前一个参数的高 4 字节当作字符串指针，然后段错误。排查方法：仔细对比格式字符串和参数列表，确保每个格式符对应的参数类型和 `va_arg` 调用的类型严格一致。在我们的实现里，`%d` 对应 `va_arg(args, int)`，`%u` 对应 `va_arg(args, unsigned int)`，`%x`/`%X`/`%p` 对应 `va_arg(args, uint64_t)`。

---

## 本章小结

| 组件 | 关键函数/类型 | 说明 |
|------|-------------|------|
| 格式化引擎核心 | `vkprintf_impl<OutputFn>()` | 泛型模板，格式解析 + 参数格式化 + 输出 |
| 十进制格式化 | `format_decimal()` | 除法取余法，INT64_MIN 特殊处理 |
| 十六进制格式化 | `format_hex()` | 位运算 `& 0xF` + 右移 4 位 |
| 初始化 | `kprintf_init()` | 初始化 g_serial (COM1, 115200 8N1) |
| 可变参数输出 | `kprintf(fmt, ...)` | 最常用的公共接口 |
| va_list 输出 | `kvprintf(fmt, va_list)` | 方便包装的 va_list 版本 |
| 内核 panic | `kpanic(fmt, ...)` | 输出错误信息后永久 halt |
| 格式符 | `%d %u %x %X %p %s %c %%` | 有符号/无符号十进制、十六进制、指针、字符串、字符、转义 |
| 宽度修饰 | `%Nd %0Nd` | 右填充空格 / 左填充零 |
| 测试 | `tests/unit/test_kprintf.cpp` | 22 个 host 端单元测试，445 行 |

这一章我们为内核构建了一个完整的格式化输出引擎。模板化的 `vkprintf_impl` 让同一套格式化逻辑既能输出到串口也能输出到测试缓冲区，INT64_MIN 的特殊处理让我们踩过了一个经典的补码陷阱，`%p` 的固定宽度输出让地址日志整齐可读。

有了 `kprintf`，我们终于可以告别手动拼字符的原始时代，用一行格式化代码就把调试信息输出到串口了。下一章（009D）我们要面对的是一个更硬核的挑战——ELF 加载器。大内核编译出来的是一个 ELF 格式的可执行文件，我们需要在 mini kernel 里解析 ELF 头、加载 PT_LOAD 段、处理 BSS 清零、转换 higher-half 入口地址，最终跳转到真正的大内核执行。有了 `kprintf` 这个得力的调试工具，那些 ELF 解析过程中的各种边界条件排查会轻松很多。

下一章见。
