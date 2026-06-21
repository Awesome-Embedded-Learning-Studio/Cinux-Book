---
title: 005 · 内核会说话了
---

# 005 · 内核会说话了:串口、kprintf 与双轨测试

> [004](../01-boot/004-boot-load-mini-kernel.md) 我们让内核跑起来了,可它只会往 debugcon 吐单字符(`P`、`L`、`===CPP`),既不能打印一个数字,也没法把 BootInfo 里那张内存图好好 dump 出来。更要命的是——我们改一行代码,除了"重跑看它崩不崩"之外没有任何验证手段。这一章,我们要给内核装上**真正的输出**(串口 + kprintf)和**真正的测试**(host 单测 + QEMU 内核测试双轨)。从这以后,内核才算"会说话",我们也才算有底气继续往上堆功能。

## 这一章我们要点亮什么

三件事,一件比一件实在。

第一,写一个**串口驱动**:让内核能往 COM1(端口 `0x3F8`)一个字节一个字节地输出文本,QEMU 的 `-serial stdio` 直接接住,终端里就能看到内核在说什么。

第二,写一个 **kprintf**——内核版的 `printf`。给它一个格式串和几个参数,它能把 `%d`、`%x`、`%p` 这些格式化好地打到串口上(顺带还支持 `%b` 二进制,这是 Cinux 自己加的)。

第三,搭一套**双轨测试**:一边是 host 上跑的单元测试(用宿主机 g++ 编,CTest 驱动),另一边是 QEMU 里跑的内核测试(和真内核同样的编译选项,跑完自动退出)。两边共享同一份纯算法代码。

做完之后,`make test` 会先在 host 上跑格式化的边界测试,再在 QEMU 里跑一遍内核,串口打印出 `=== All tests completed ===` 后干净退出。整个内核第一次有了"改完能自动验证"的能力。

## 为什么现在需要它

004 的内核有个很尴尬的处境:它已经能跑 C++ 了,可一旦想确认"我的 BootInfo 读对了没"、"那张 E820 内存图有几条、各多大",你毫无办法。debugcon 只能吐单字符,想打个 `42` 都得自己拆位。没有结构化输出,后面的内存管理、进程这些东西根本没法调试——你连"分配了多少"都打印不出来。

但更深的问题不是输出,是**测试**。到目前为止,我们验证内核的方式只有一种:重编、重跑、看它崩不崩。这种验证粗糙得可怕——一个边界 bug(比如 `format_decimal` 遇到 `INT64_MIN` 直接溢出)可能要等到很久以后某个偶然的场景才暴露。我们需要的,是把内核里那些**和硬件无关的纯算法**(比如"把一个整数转成字符串")单独拎出来,在 host 上用正常的测试框架去磨它。

这正好串起了这一章的核心设计。串口和 kprintf 解决"怎么输出";而 kprintf 之所以能做到 host 可测,是因为我们把"格式化算法"(`format.cpp`)和"输出到哪"(`serial` 还是 `debugcon`)解耦了——那个算法是纯函数,既能编进内核,也能编进 host 测试。这就是这一章最值钱的一个架构决定。

> 外部依据:OSDev 的 Serial Ports 页描述了 16550 UART 的寄存器布局与 LSR 状态位;PC 的 COM 端口标准(COM1 基址 `0x3F8`)是 IBM PC 定下的约定。

## 设计图

先看串口这一层。一个 UART(NS16550A)挂在一段连续的 I/O 端口上,基址 `0x3F8`,各寄存器按偏移区分:

```text
偏移   寄存器   读/写    用途
 0     RBR/THR  读/写    收/发缓冲(同一个偏移,靠读/写区分)
 1     IER      写       中断使能(我们关掉,轮询)
 2     FCR      写       FIFO 控制
 3     LCR      写       线路控制(8N1 = 0x03)
 4     MCR      写       Modem 控制(RTS+DTR = 0x03)
 5     LSR      读       线路状态(bit5=可发, bit0=可收)
```

发一个字符的流程就是死循环查 LSR 的 bit5(发送保持寄存器空了没),空了就往 THR(offset 0)写字节。收字符类似,查 bit0。

再看 kprintf 怎么把"格式化"和"输出"解耦。关键是模板加一个输出函数对象:

```text
vkprintf_impl<OutputFn>(putc, format, args)
   ├─ 遍历 format 串,遇 % 走格式化分支
   ├─ 数字/指针 → 调 format.cpp 的纯函数算出字符串
   └─ 每个字符 → 调 putc(c)            ← 输出门户在这里抽象掉
        ├─ kprintf:  putc = serial.putc   (打到 COM1)
        └─ kdebugf:  putc = debugcon_putc (打到 0xE9)
```

`OutputFn` 是个抽象:你给它一个"怎么吐一个字符"的函数,`vkprintf_impl` 只管把格式化好的字符逐个喂给它。于是同一套格式化逻辑,串口、debugcon、甚至以后接帧缓冲,都只是换个 `putc`。

而双轨测试的纽带,就是中间那个 `format.cpp`:

```text
              format.cpp(纯算法:format_decimal/hex/binary)
              ┌────────────────────┴────────────────────┐
        编进内核                                  编进 host 测试
   kprintf.cpp 调它                      test_kprintf_format.cpp 调它
   (走 serial 输出)                      (走 ASSERT_EQ 比对字符串)
        │                                          │
   mini_kernel(QEMU 跑)                     test_host(CTest 跑)
```

同一份 `format.cpp`,两个编译上下文:内核里它被 kprintf 调用输出到串口;host 上它被单元测试调用、结果拿去和期望字符串比对。算法只有一份,内核和测试不会各写各的。

## 代码路线

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

`pause` 是给 CPU 的提示:"我在自旋等,你稍微省点电、也别让乱序拖累"。这里**故意不开中断**(`IER=0`):这一章的串口是"我说你听"的单向输出通道,中断驱动的收发是后面 [007](007-mini-kernel-intr.md) 的事。`puts` 还做一件小事:遇到 `\n` 先补一个 `\r`——串口终端把 `\n` 当"换行"不回车,不补 `\r` 的话每行会逐行往右错位(经典"阶梯状"输出)。

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

## 调试现场

这一章值得记的真实坑,都和"边界"或"工具链"有关。

第一个是上面讲的 `INT64_MIN`。`format_decimal` 不特判它,`-value` 溢出,打印 `INT64_MIN` 会得到一串错的数字。这种 bug 在内核里极难触发(谁会专门打印 `INT64_MIN`?),但 host 单测一条就抓出来。这也是为什么"纯算法要能 host 测"——它不是为了好看,是真抓 bug。

第二个是串口的 Baud / 配置。`init` 里设的 `LCR=0x03`(8N1)必须和 QEMU `-serial` 默认的 115200 8N1 对上。配错一位,终端收到的就是满屏乱码——能看出"有东西在发",但全是垃圾。判断方法:先发一个固定字符(比如 `A`),终端看到 `A` 就说明线路配置对,看到乱码就是 LCR/Baud 问题。

第三个是 `puts` 不转 `\r\n`。漏了 `\n` 前 `putc('\r')` 的话,终端每换行不回首列,输出会呈阶梯状斜着走。这是串口输出的经典初见坑,一眼能认。

第四个(也是 006 要再次踩的)是对象库与全局构造。`format.cpp` 被单独编成静态库再链进内核,如果链接/构造函数表没处理好,全局对象的构造可能不被调用。这一章的 `linker.ld` 特意用 `KEEP(*(.init_array))` 防止 `.init_array` 被链接器当垃圾裁掉——裁掉了,`_init_global_ctors` 遍历到的就是空,全局对象构造全跳过。`test_cpp_basic` 里那个"全局对象构造"测试,就是专门盯这个的。

## 验证

这是本系列第一次有真正的自动化测试,验证也第一次分两条路。

host 单测(快,CI 友好):

```bash
cmake --build build --target test_host
```

它会跑 CTest,测 `format_*` 的各种边界。全过的话终端会报告 `kprintf_format` 等 test 通过。

QEMU 内核测试:

```bash
cmake --build build --target run-kernel-test   # 跑 mini_kernel_test,自动退出
```

串口上会依次看到 `=== kprintf Test ===`(各种格式化样例)、`=== C++ Runtime Tests ===`(四个 `[RUN]/[PASS]`),最后 `=== All tests completed ===`,然后 QEMU 靠 `0xf4` 退出。退出码 0 就是全过。

一次跑全套:

```bash
cmake --build build --target test   # 先 host,后 kernel
```

想看生产内核(非测试版)长什么样,`make run` 会跑量产 `mini_kernel.bin`,串口打印 `Cinux Mini Kernel v0.1.0`、BootInfo、还有那张 E820 内存图的逐条 dump——这条 dump 正是下一章内存管理的原料。

## 下一站

内核现在会说话了:能往串口打格式化文本,改完代码还有双轨测试兜底。可你看量产内核 dump 出的那张 E820 内存图——它只是**打印**出来了,内核根本还没用它。`operator new` 调一下还是原地 `hlt`,因为我们既没有物理内存管理,也没有堆。

下一章 [006 · 物理内存管理(PMM)](006-mini-kernel-pmm.md),我们就要把那张内存图真正用起来:建一个位图,标记哪些物理页可用、哪些已分出去,给内核一个能分配/回收物理页的分配器。从那以后,内核才算开始"管资源",而不只是"会说话"。

---

### 参考

- OSDev — [Serial Ports](https://wiki.osdev.org/Serial_Ports)(16550 UART 寄存器、LSR 状态位、8N1 配置)、[ISA debug exit device](https://wiki.osdev.org/ISA_debug_exit_device)(端口 `0xf4` 退出机制)。
- PC/AT 硬件标准 — COM 端口基址约定(COM1=`0x3F8`)。
- cppreference — `va_list`/`va_arg`(可变参数机制)、`INT64_MIN`/`INT64_MAX` 边界。
- 本 tag 源码:[serial.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/driver/serial.cpp)/[serial.h](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/driver/serial.h)/[io.h](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/driver/io.h)、[kprintf.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/lib/kprintf.cpp)/[format.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/lib/private/format.cpp)、[main_test.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/test/main_test.cpp)/[test_cpp_basic.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/test/test_cpp_basic.cpp)、[test_kprintf_format.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_kprintf_format.cpp)、[linker.ld](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/linker.ld)。
- 调试素材提炼自 [005](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/document/notes/005/)(测试框架、kprintf 算法、调试工作流、mistake-check)。

> 参考 URL 的有效性会在全局审查阶段用 open-websearch(bing)统一核活。
