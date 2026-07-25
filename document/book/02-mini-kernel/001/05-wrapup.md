---
title: 05 · 验证与下一站
---

# 验证与下一站

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

下一章 [002 · 物理内存管理(PMM)](../002/),我们就要把那张内存图真正用起来:建一个位图,标记哪些物理页可用、哪些已分出去,给内核一个能分配/回收物理页的分配器。从那以后,内核才算开始"管资源",而不只是"会说话"。

---

### 参考

- OSDev — [Serial Ports](https://wiki.osdev.org/Serial_Ports)(16550 UART 寄存器、LSR 状态位、8N1 配置)、[ISA debug exit device](https://wiki.osdev.org/ISA_debug_exit_device)(端口 `0xf4` 退出机制)。
- PC/AT 硬件标准 — COM 端口基址约定(COM1=`0x3F8`)。
- cppreference — `va_list`/`va_arg`(可变参数机制)、`INT64_MIN`/`INT64_MAX` 边界。
- 本 tag 源码:[serial.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/driver/serial.cpp)/[serial.h](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/driver/serial.h)/[io.h](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/driver/io.h)、[kprintf.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/lib/kprintf.cpp)/[format.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/lib/private/format.cpp)、[main_test.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/test/main_test.cpp)/[test_cpp_basic.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/test/test_cpp_basic.cpp)、[test_kprintf_format.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_kprintf_format.cpp)、[linker.ld](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/linker.ld)。
- 调试素材提炼自 [005](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/document/notes/005/)(测试框架、kprintf 算法、调试工作流、mistake-check)。
