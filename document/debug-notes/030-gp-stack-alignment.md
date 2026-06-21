---
title: Debug · 030 开机即 #GP:一个潜伏已久的 ISR 栈对齐 bug
---

# Debug · 030 开机即 #GP:一个潜伏已久的 ISR 栈对齐 bug

> 出处:tag `030_gui_wm_basic`,`document/notes/030/gp_fault_stack_alignment.md`。这里按「症状 → 定位 → 根因 → 修复 → 防复发」提炼,不照抄原始笔记。地址/常量以 tag 源码为准。

这次排错最反直觉的地方:**崩溃点根本不在我们这一章动的代码里**。030 给内核接上 PS/2 鼠标、搭窗口管理器,一开机却炸在 014 就写好、一直好好的**键盘** IRQ1 handler 上。顺着这条线追下去,根因是一个从写第一个 ISR 起就埋下的栈对齐 bug——它静默地潜伏了十几个 tag,直到这一章某段无害的代码恰好让编译器生成了一条对齐敏感的指令,才被顶出水面。

## 症状

`CINUX_GUI=ON` 构建、`make run` 启动,刚打印出里程碑:

```text
[GUI] ===== Milestone 030: GUI Window Manager =====
```

紧接着立刻触发 `#GP`(General Protection,vector 13):

```text
==== EXCEPTION: #GP (vector 13) ====
  RIP   = 0xFFFFFFFF81001DBB   CS  = 0x0010
  RFLAGS= 0x0000000000010002
  RSP   = 0xFFFF800008047EF8   SS  = 0x0018
```

诡异的是 RIP 指向的崩溃指令在键盘的 `irq1_handler` 里,而不是鼠标代码。而且 `CINUX_GUI=OFF` 时全部基线测试通过——说明这个 bug 只在 GUI 构建下才炸。

## 定位

崩溃指令是一条 `movaps %xmm0, (%rsp)`——SSE 指令,要求操作数地址 **16 字节对齐**,不对齐就 `#GP`。问题来了:为什么一个一直存在的键盘 handler,现在才生成 `movaps`?

把调用链捋清楚:

```text
gui_start()
  → Mouse::init()            // 操作 8042 PS/2 控制器(0xA8 / 0x20 / 0x60 / 0xD4 / 0xF4)
    → 控制器状态翻转,顺带产生一个虚假的 IRQ1(键盘中断)   // PS/2 控制器的已知副作用
      → irq1_stub
        → Keyboard::irq1_handler()
          → GUI 双路分发:cinux::drivers::Mouse::event_queue().enqueue(gui_ev)
            → 编译器为这段优化,动用 XMM 寄存器 → movaps %xmm0, (%rsp)   // ← #GP
```

两个关键点叠加,才把 bug 顶出来:

一是 `Mouse::init()` 走 8042 命令序列启用 AUX 口时,控制器状态翻转会产生一个**虚假的 IRQ1**——这是 PS/2 硬件的已知行为,不是我们的代码触发的中断,但 ISR 必须能安全处理它。

二是这一章给 `irq1_handler` 加了 GUI 双路分发(把键盘事件额外拷一份进 GUI 事件队列)。这段代码本身人畜无害,但它让编译器在 handler 里用上了 XMM 寄存器,生成了那条要求 16 字节对齐的 `movaps`。而此刻进入 handler 时 `(%rsp)` 没对齐——所以炸。

## 根因

`movaps` 没对齐只是表象,根因是 **ISR stub 违反了 x86_64 System V ABI 的栈对齐规则**。这条规则要求:进入一个函数的瞬间,`RSP ≡ 8 (mod 16)`,即 `(RSP + 8)` 是 16 的倍数(System V AMD64 ABI §3.2.2「The Stack Frame」)。编译器就靠这个约定,才敢放心生成 `movaps` 这类要求 16 字节对齐的指令。

而我们的 ISR stub 原来不满足它。看修复**前**的栈账(以无错误码的 IRQ 为例):

```text
CPU 自动压入(IRQ 无错误码): SS, RSP, RFLAGS, CS, RIP = 5 × 8 = 40 字节
ISR stub 压入:               假错误码 + rax..r15 = 16 × 8 = 128 字节
                                                        合计 168 字节
call handler 压入返回地址:                                   8 字节
                                                        合计 176 字节
```

176 是 16 的倍数,意味着 `call` 之后进入 handler 的瞬间 **RSP ≡ 0 (mod 16)**——和 ABI 要求的 `RSP ≡ 8` 正好差 8 字节。handler 内部再 `push %rbx; sub $0x20,%rsp` 调整栈帧后,落到那条 `movaps` 时地址恰好没对齐,`#GP` 触发。

## 修复

在压完 GPR 后、`call` 之前,额外 `push $0` 垫 8 字节对齐 padding(`kernel/arch/x86_64/interrupts.S` 里的 `ISR_NOERRCODE` / `ISR_ERRCODE` 两个宏都改):

```asm
    pushq %r15
    pushq $0                 # 对齐 padding(8 字节)
    leaq 8(%rsp), %rdi       # InterruptFrame* 跳过 padding,仍指向原来的 r15 字段
    call \handler
    addq $8, %rsp            # 弹掉 padding
    popq %r15                # 按原顺序恢复 GPR
```

加了这 8 字节后:40 + 128 + 8(padding) = 176,`call` 再压 8 = **184**,`184 ≡ 8 (mod 16)` ✓,handler 入口栈对齐正确,`movaps` 不再炸。

两处必须小心:一是 `leaq 8(%rsp), %rdi`——padding 是临时垫的,传给 C handler 的 `InterruptFrame*` 必须跳过它、仍指向原来的 `r15` 字段,这样 `InterruptFrame` 结构体布局完全不用改;二是恢复时先 `addq $8` 弹掉 padding,再按原顺序 `pop` GPR,顺序不能错。

> 一个 bug 引出一个链接符号。修完 `#GP`,链接器接着报 `__dso_handle` 未定义:`WindowManager::instance()` 里那个 `static WindowManager wm;` 单例**带析构函数**,编译器要把它通过 `__cxa_atexit(func, arg, __dso_handle)` 注册成退出时调用的析构。我们的 freestanding 内核没有动态链接,得自己提供这个符号——在 `kernel/arch/x86_64/crt_stub.cpp` 补一个 `void* __dso_handle = nullptr;` 就够了(内核没有 DSO,空指针足矣)。这是「从零搭 GUI」这类大改动典型的连带效应。

## 防复发

**ISR stub 必须保证 handler 入口 `RSP ≡ 8 (mod 16)`,这是 System V ABI 的硬性要求,不是可选项。** 写 ISR 时把那笔栈账算一遍:CPU 压入的字节数 + stub 压入的字节数,`call` 之后再 mod 16,必须落在 8。带错误码的异常 CPU 会多压一个错误码,账要重算,但结论一样——不够 8 就补 padding。

更深的教训有两条。一是**栈对齐 bug 是静默的**:简单 handler 不触发,只有编译器恰好生成对齐敏感指令(SSE `movaps`/`movdqa` 等)时才暴露,排查难度高。在 `-O2` 下、handler 里有稍复杂的逻辑时尤其要警惕。二是**硬件副作用可能触发意外中断**:操作 8042 PS/2 控制器会产生虚假 IRQ,ISR 必须随时能安全执行——这反过来要求所有 ISR 从一开始就满足 ABI 对齐,不能心存侥幸。

> 相关但不同性质:030 还有个「不是 bug」的硬件特性——QEMU VNC 里同时出现两个光标(QEMU 圆点 + 我们画的箭头)且有固定偏移。根因是 PS/2 协议只报告相对位移、VNC 宿主光标用绝对坐标,两者起点不同(`document/notes/030/mouse_cursor_offset.md`)。缓解是 QEMU 加 `-usb -device usb-tablet` 让宿主光标改用绝对定位、guest 初始位置设 `(0,0)`。彻底解法是写 USB HID 绝对坐标驱动——那超出 030 范围。遇到「两个光标对不上」,先别怀疑绘图代码,查输入协议本身能不能给绝对坐标。

---

## 参考

- System V Application Binary Interface — AMD64 Architecture Processor Supplement,§3.2.2 The Stack Frame(函数入口 `(%rsp + 8)` 必须是 16 的倍数):<https://gitlab.com/x86-psABIs/x86-64-ABI>
- Intel SDM Vol. 1 / Vol. 2:`movaps` 等 SSE 指令要求 16 字节对齐操作数,未对齐触发 `#GP`(vector 13)。
- OSDev Wiki — "8042" PS/2 Controller(`0xA8 / 0x20 / 0x60 / 0xD4 + 0xF4` 命令序列及其产生虚假中断的副作用):<https://wiki.osdev.org/%228042%22_PS/2_Controller>
- 原始排查笔记:[gp_fault_stack_alignment.md](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/document/notes/030/gp_fault_stack_alignment.md)、[mouse_cursor_offset.md](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/document/notes/030/mouse_cursor_offset.md)。
