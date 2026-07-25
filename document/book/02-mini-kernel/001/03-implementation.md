---
title: 03 · 代码路线:串口、kprintf、format、双轨测试
---

# 代码路线:串口、kprintf、format、双轨测试

### 1. 串口驱动:轮询式 UART

最底层的 I/O 原语是两条内联汇编——读/写一个字节到指定端口([io.h](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/driver/io.h)):

```cpp
inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}
```

x86 用 `in`/`out` 指令访问 I/O 端口空间(这和内存是两套地址空间,不能拿指针解引用去碰)。`"=a"` 把结果放进 `al`,`"Nd"` 让端口用立即数或 `dx` 传。

[serial.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/driver/serial.cpp) 的 `Serial` 类把 UART 包起来。构造时先 `init` 配成 8N1:

```cpp
void Serial::init() {
    io::outb(base_port + IER, 0x00);   // 关中断:我们轮询,不要 UART 中断
    io::outb(base_port + LCR, 0x03);   // 8 数据位、无校验、1 停止位
    io::outb(base_port + FCR, 0xC7);   // 开 FIFO、清缓冲、14 字节阈值
    io::outb(base_port + MCR, 0x03);   // RTS + DTR
}
```

发字符是轮询的精髓——`putc` 先死等 LSR 的 bit5(TX_READY)置位,表示发送保持寄存器空了,再把字节塞进 THR:

```cpp
void Serial::putc(char c) {
    while (!is_tx_ready()) __asm__ volatile("pause");  // 自旋等
    io::outb(base_port + THR, static_cast<uint8_t>(c));
}
```

`pause` 是给 CPU 的提示:"我在自旋等,你稍微省点电、也别让乱序拖累"。这里**故意不开中断**(`IER=0`):这一章的串口是"我说你听"的单向输出通道,中断驱动的收发是后面 [003](../003/) 的事。`puts` 还做一件小事:遇到 `\n` 先补一个 `\r`——串口终端把 `\n` 当"换行"不回车,不补 `\r` 的话每行会逐行往右错位(经典"阶梯状"输出)。

构造函数里还埋了一串 debugcon 面包屑:`init` 前打 `\`、`init` 各步打 `[1 2 3 4`、`init` 后打 `'`。这些是给串口本身还没通时的"调试串口的调试"——万一串口初始化卡在某一步,debugcon 上能看到卡在哪个数字,比黑屏强。

### 2. kprintf:把"格式化"和"输出目的地"解耦

[kprintf.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/lib/kprintf.cpp) 的核心是一个模板函数,接受一个"吐一个字符"的函数对象:

```cpp
template <typename OutputFn>
void vkprintf_impl(OutputFn&& putc, const char* format, va_list args) {
    char buffer[64];
    while (*format) {
        if (*format == '%') {
            // 解析 % [0] [width] type,调 format_* 算出字符串,putc 逐字输出
        } else {
            putc(*format++);
        }
    }
}
```

然后两个对外接口,区别只在"吐到哪":

```cpp
void kprintf(const char* fmt, ...) {  // → 串口
    va_list args; va_start(args, fmt);
    auto& serial = serial::get_initial_serial();
    vkprintf_impl([&](char c){ serial.putc(c); }, fmt, args);
    va_end(args);
}
void kdebugf(const char* fmt, ...) {  // → debugcon 0xE9
    va_list args; va_start(args, fmt);
    vkprintf_impl([](char c){ debugcon_putc(c); }, fmt, args);
    va_end(args);
}
```

为什么费这个劲搞模板,而不是直接写两个几乎一样的函数?因为格式化的逻辑(`%d` 怎么转、宽度怎么补)很复杂且容易出错,我们**绝对不想写两份**。模板让"格式化"只存在一份,"输出到哪"作为一个参数注入。以后想加帧缓冲输出,也是再加一个 `kprintf` 变体、传个写像素的 `putc` 进去,格式化那几十行一个字不用动。

支持的格式是 Cinux 自己挑的一套精简版:`%%`、`%c`、`%s`、`%d`、`%u`、`%x`/`%X`、`%p`(带 `0x` 前缀)、还有个 `%b`(二进制,调试位掩码时很顺手),外加 `%N`/`%0N` 的宽度填充。它**不是**完整 printf——没有浮点、没有精度、没有 `%l` 长度修饰。够用就好,内核不需要 `printf("%f", 3.14)`。

### 3. format.cpp 为什么单独抽出来

`vkprintf_impl` 里真正把数字变成字符串的那几个函数——`format_decimal`、`format_hex`、`format_binary`——被放在单独的 [format.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/lib/private/format.cpp),还单独编成一个静态库(`kprintf_private`)。这看似多余,实则是整个可测性设计的命门。

看看 `format_decimal` 里一个真实的坑就懂了:

```cpp
int format_decimal(int64_t value, char* buffer, int buffer_size) {
    bool is_neg = value < 0;
    if (is_neg) {
        if (value == INT64_MIN) {                    // ★ 特判
            // 直接拷 "-9223372036854775808"
            ...
        }
        value = -value;                              // 否则这里溢出!
    }
    ...
}
```

`INT64_MIN` 是 `-9223372036854775808`,它的绝对值比 `INT64_MAX` 大 1,`-value` 会溢出成它自己(还是负数),后面整个转换就乱了。这种边界,你要是只在 QEMU 里跑、只在恰好打印 `INT64_MIN` 时才触发,可能永远发现不了。但因为 `format.cpp` 是**纯函数**(输入一个数、输出一串字符,不碰任何硬件、不调任何 I/O),我们完全可以把它编进 host 测试,直接 `ASSERT_EQ(format_decimal(INT64_MIN, ...), "-9223372036854775808")`——一条测试就把这个坑钉死。

这就是"纯逻辑单独抽库"的全部回报:凡是和硬件无关的算法,都值得让它能脱离内核、在 host 上被磨。`format_hex` 去前导零、`format_binary` 跳过高位 0,这些也都是同类的纯逻辑,一并放进 host 测试覆盖。

### 4. 双轨测试:host CTest + QEMU 内核测试

两条测试轨道,各管一摊。

**host 轨道**([test_kprintf_format.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_kprintf_format.cpp))测的是上一节那些纯函数。它直接 `#include "mini/lib/private/format.h"`,把 `format.cpp` 跟测试一起用宿主机 g++ 编(加 `-DCINUX_HOST_TEST` 告诉代码"现在跑在 host 上"),用一套自研的轻量宏(`TEST(...)`、`ASSERT_EQ`、`RUN_ALL_TESTS`)断言。测试覆盖正负零、`INT64_MIN`/`INT64_MAX`、hex 全数字、binary 去前导零这些边界。跑法是 CTest:`cmake --build build --target test_host`。这条轨道快、能在 CI 里跑、不依赖 QEMU,是日常改格式化代码的第一道闸。

**QEMU 轨道**([test](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/test/))测的是"真内核里能不能跑"。它构造一个 `mini_kernel_test` 目标,和量产内核用**完全一样的编译/链接选项**,只是把 `main.cpp` 换成测试专用的 `main_test.cpp`,再加上 `test_cpp_basic.cpp`。后者用另一套自研宏(`TEST_ASSERT`、`RUN_TEST`)测 C++ 运行时本身:类的构造/析构计数对不对、虚函数派发对不对、全局对象构造函数跑没跑、多重继承的 `this` 调整对不对。这些必须在真内核里跑(它们依赖 vtable、`.init_array`、链接脚本),host 测不了。

内核测试怎么"知道跑完了并报告结果"?靠 QEMU 的 isa-debug-exit 设备:测完往端口 `0xf4` 写一个双字,QEMU 就用那个值当退出码直接退出。于是 CI 能拿到退出码判断过没过,不用人去盯串口。两条轨道最后被 `make test` 串起来:先 host 后 kernel。

这套双轨,本质上是按"能不能脱离硬件"把测试劈成两半——能脱离的(host)、必须真硬件的(kernel),各走最快的路。
