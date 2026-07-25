---
title: 05 · 验证与下一站
---

# 验证与下一站

这一章有两套验证,各管一层。

host 单测验"编码对不对":

```bash
cmake --build build --target test_host
```

`test_gdt_idt`(六百多行)专门测 GDT/IDT 的字节级编码——`make_gdt_entry` 出来的描述符字节对不对、gate 的 `type_attr` 算对没、选择子值对不对。这些是纯逻辑(给定 access/flags,算出 8 字节描述符),适合 host 磨,不用启动 QEMU。

QEMU 内核测验"真能跑":

```bash
cmake --build build --target run-kernel-test
```

`test_interrupts` 会真触发异常,确认 handler 跑了、寄存器 dump 出来、内核存活。

量产内核看效果:

```bash
cmake --build build --target run
```

串口依次出现 `[INIT] Setting up GDT...`、`GDT loaded`、`Setting up IDT...`、`IDT loaded`,PMM 统计,然后 `[TEST] Triggering breakpoint exception (int $3)...`,紧跟着 `==== EXCEPTION: #BP (vector 3) ====` 和一整块寄存器,最后 `[TEST] Breakpoint test passed! Execution continued after #BP.`。看到"continued after #BP",说明异常被接住、`iretq` 正确返回、内核活着。

## 下一站

mini kernel 现在有了内存(PMM)、会说话(串口/kprintf)、还能被异常打断并自愈。可它终究是个"mini"——它是为引导一个**更大的内核**做准备的跳板。那个真正完整的 big kernel(有完整 GDT/TSS、有驱动、有进程)还没登场。

下一章 [004 · 加载大内核](../004/),mini kernel 要从磁盘把那个 big kernel 的 ELF 镜像读进来、解析、加载到该去的位置,然后把控制权交过去。mini kernel 由此完成它"从 bootloader 手里接力、再把棒交给 big kernel"的全部使命。从那以后,Cinux 的主角就换成 big kernel 了。

---

### 参考

- OSDev — [Interrupt Descriptor Table](https://wiki.osdev.org/Interrupt_Descriptor_Table)(64 位门描述符格式)、[Exceptions](https://wiki.osdev.org/Exceptions)(#BP/#PF 语义与错误码各位)、[Interrupt Service Routines](https://wiki.osdev.org/Interrupt_Service_Routines)(ISR stub 栈帧约定)。
- Intel SDM Vol.3A — IDT 门描述符(中断门/陷阱门与 IF 行为)、#BP/#PF 异常、CR2、页错误码位定义。(章节号以本地 2023-06 版标题为准。)
- 本 tag 源码:[gdt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/arch/x86_64/gdt.cpp)/[gdt.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/arch/x86_64/gdt.hpp)、[idt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/arch/x86_64/idt.cpp)/[idt.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/arch/x86_64/idt.hpp)、[interrupts.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/arch/x86_64/interrupts.S)、[exception_handlers.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/arch/x86_64/exception_handlers.cpp)、[test_gdt_idt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_gdt_idt.cpp)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/main.cpp)。
