---
title: Lab 005 · 内核会说话了
---

# Lab 005 · 内核会说话了

> 这个 lab 配套 [005 · 内核会说话了](../../book/02-mini-kernel/005-mini-kernel-entry.md)。目标:给 mini kernel 写串口驱动 + kprintf,并给格式化算法配一套 host 单测,再在 QEMU 里跑内核测试。**UART 寄存器自己配、格式化算法自己写、测试断言自己定**,不给现成答案。

## 实验目标

- 写 `inb`/`outb` 原语和 `Serial` 类(COM1、轮询、8N1),`putc`/`puts` 能在 QEMU 串口打出文本。
- 写 `kprintf`,用模板把"格式化"和"输出后端"解耦,串口和 debugcon 各一个后端;支持 `%d/%x/%p/%s/%b` 等。
- 把数字→字符串的纯算法抽进 `format.cpp`,并给它配 host 单测(CTest),覆盖 `INT64_MIN` 等边界。
- 搭 QEMU 内核测试目标,用 isa-debug-exit(`0xf4`)退出,测 C++ 运行时。

## 前置条件

- 完成 [Lab 004](../01-boot/lab-004-load-mini-kernel.md):内核能在长模式跑 C++、有 BootInfo。
- 会写 freestanding C++、看得懂 `va_list`/`va_arg`、会用 inline 汇编包 `in`/`out`。

## 任务分解

分五块走。

第一块,I/O 原语和串口。先写 `io.h` 的 `inb`/`outb`(inline 汇编,`"=a"`/`"Nd"` 约束)。再写 `Serial`:构造时 `init`(IER=0 关中断、LCR=0x03 八位无校验一停止、FCR=0xC7 开 FIFO、MCR=0x03);`putc` 自旋等 LSR bit5 再写 THR;`puts` 在 `\n` 前补 `\r`。提供一个全局单例。

第二块,kprintf。写模板 `vkprintf_impl<OutputFn>(putc, fmt, args)`:遍历格式串,遇 `%` 解析可选 `0` 和宽度数字、再读类型字母,调 `format_*` 算字符串后逐字 `putc`。再封两个接口:`kprintf` 传串口的 putc、`kdebugf` 传 debugcon 的 putc。

第三块,format.cpp。写 `format_decimal`(注意 `INT64_MIN` 特判,否则 `-value` 溢出)、`format_hex`(大小写、去前导零)、`format_binary`(跳过高位的 0)。单独编一个静态库。

第四块,host 单测。写 `test/unit/test_kprintf_format.cpp`,直接 include `format.h`、把 `format.cpp` 跟测试一起用 host g++ + `-DCINUX_HOST_TEST` 编,用自研 `TEST/ASSERT_EQ` 断言各种边界(0、正负、`INT64_MIN`/`MAX`、hex 全数字、binary 去前导零)。注册到 CTest。

第五块,QEMU 内核测试。构造一个和量产内核同编译选项的 `mini_kernel_test` 目标(换测试用 main、加 C++ 运行时测试),测完往端口 `0xf4` 写双字退出。

## 接口约束

这些得自己保证对、lab 不给现成代码:

- 串口:COM1 基址 `0x3F8`;寄存器偏移 RBR/THR=0、IER=1、FCR=2、LCR=3、MCR=4、LSR=5;LSR 位 `TX_READY=0x20`(bit5)、`RX_READY=0x01`(bit0);init 用 `LCR=0x03`/`FCR=0xC7`/`MCR=0x03`。
- kprintf:第一参数走 `va_list`;格式串里 `%` 后可选 `0` 和十进制宽度;`%p` 要带 `0x` 前缀;未知 `%X` 原样吐出 `%` 加该字符。
- format 签名:`int format_decimal(int64_t, char*, int)`、`int format_hex(uint64_t, char*, int, bool lowercase)`、`int format_binary(uint64_t, char*, int)`,返回写入字符数(不含 `\0`)。
- 内核测试退出:往端口 `0xf4` `outl` 一个双字,QEMU 用其值退出;退出码 0 = 全过。
- 风格:轮询、无中断(`IER=0`);这是输出通道,收/中断是后面的事。

## 验证步骤

host 单测(第一道,快):

```bash
cmake --build build --target test_host
```

QEMU 内核测试(第二道):

```bash
cmake --build build --target run-kernel-test
```

串口应依次出现 `=== kprintf Test ===`、`=== C++ Runtime Tests ===` 的 `[RUN]/[PASS]`、最后 `=== All tests completed ===`,QEMU 以退出码 0 退出。

一次跑全套:

```bash
cmake --build build --target test   # 先 host,后 kernel
```

量产内核看效果:`make run`,串口打印 `Cinux Mini Kernel v0.1.0` + BootInfo + E820 内存图逐条 dump。

## 常见故障

串口满屏乱码、能看出有字符在发但全是垃圾。多半是 LCR/Baud 配置和 QEMU 默认(115200 8N1)对不上。先发个固定 `A` 看终端收不收得到对的。

每换行就往右错位、呈阶梯状。`puts` 漏了 `\n` 前补 `\r`。

打印大负数(比如 `INT64_MIN`)得到错值。`format_decimal` 没特判 `INT64_MIN`,`-value` 溢出。host 单测一条就能抓出来——这正是单测存在的意义。

改了格式化代码、host 测试却没覆盖到新行为。给 `format.cpp` 的每条新逻辑都补一条 host 断言;别只靠 QEMU 跑(慢、覆盖不到边界)。

内核测试里全局对象的构造没跑(全局构造测试挂)。`linker.ld` 没用 `KEEP(*(.init_array))`,`.init_array` 被链接器裁了。补上 KEEP。

QEMU 内核测试跑完不退出、卡在 hlt。没接 isa-debug-exit,或没往 `0xf4` 写。确认 qemu 启动参数带 isa-debug-exit 设备、测试末尾 `outl $0xf4`。

## 通过标准

- `test_host` 全过(format_* 边界覆盖)。
- `run-kernel-test` 串口出现完整测试序列,退出码 0。
- 量产内核 `make run` 能在串口看到 `Cinux Mini Kernel` 和 E820 dump。
- 全程轮询、无中断;没碰 PMM/堆——那是 [Lab 006](lab-006-mini-kernel-pmm.md) 的事。
