---
title: 03 · 调试现场与收尾
---

# 调试现场与收尾

## 调试现场

这一章的两个调试故事,一个硬核到值得单独成节,一个则是个「不是 bug 但必须懂」的硬件特性。它们都来自 `document/notes/030/` 下真实记录的踩坑。

### #GP:一个潜伏很久的栈对齐 bug,被鼠标初始化引爆

**现象。** `make run` 启动,刚打印出 `[GUI] ===== Milestone 030: GUI Window Manager =====`,立刻炸一个 `#GP`(General Protection,vector 13):

```text
==== EXCEPTION: #GP (vector 13) ====
  RIP   = 0xFFFFFFFF81001DBB   CS  = 0x0010
  RFLAGS= 0x0000000000010002
  RSP   = 0xFFFF800008047EF8   ...
```

崩溃点不在鼠标代码里,而在**键盘**的 IRQ1 handler。这就很奇怪了——我们这一章动的是鼠标,键盘 014 就写好了、一直好好的,怎么现在炸?

**触发链。** 顺着调用关系捋:`gui_start()` → `Mouse::init()` 去操作 8042 PS/2 控制器(发 `0xA8 / 0x20 / 0x60 / 0xD4 + 0xF4`)。这些控制器命令会让 8042 的状态翻转,**顺带产生一个虚假的 IRQ1**(键盘中断)——这是 PS/2 控制器的已知副作用。于是 CPU 跳进 `irq1_stub` → `Keyboard::irq1_handler()`。而在这一章,这个 handler 里多了一段 GUI 双路分发代码(往 `Mouse::event_queue()` 里 enqueue)。编译器为了优化这段,动用了 XMM 寄存器,生成了一条 `movaps %xmm0, (%rsp)`。`movaps` 要求操作数地址 **16 字节对齐**,而此刻 `(%rsp)` 没对齐,于是 `#GP`。

**根因:x86_64 的 System V ABI 栈对齐规则。** 这条规则要求:进入一个函数的瞬间,`RSP ≡ 8 (mod 16)`——也就是 `(RSP + 8)` 是 16 的倍数(见 System V AMD64 ABI §3.2.2「The Stack Frame」)。编译器就靠这个约定,才敢放心地生成 `movaps` 这种要求 16 字节对齐的 SSE 指令。

我们的 ISR stub 原来没满足它。看 [interrupts.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/interrupts.S) 修复**前**的栈账(以无错误码的 IRQ 为例):

```text
CPU 自动压入(IRQ 无错误码): SS, RSP, RFLAGS, CS, RIP = 5 × 8 = 40 字节
ISR stub 压入:               假错误码 + rax..r15 = 16 × 8 = 128 字节
                                                          合计 168 字节
call handler 压入返回地址:                                   8 字节
                                                          合计 176 字节
```

176 是 16 的倍数,意味着 `call` 之后进入 handler 的瞬间 **RSP ≡ 0 (mod 16)**——和 ABI 要求的 `RSP ≡ 8` 差了 8 字节。handler 内部再 `push %rbx; sub $0x20, %rsp` 调整栈帧后,落到那条 `movaps` 时地址正好没对齐,`#GP` 触发。

**修复:补 8 字节对齐 padding。** 在压完 GPR 后、`call` 之前,额外 `push $0` 垫 8 字节:

```asm
    pushq %r15
    pushq $0                 # 对齐 padding(8 字节)
    leaq 8(%rsp), %rdi       # InterruptFrame* 跳过 padding,仍指向 r15
    call \handler
    addq $8, %rsp            # 弹掉 padding
    popq %r15                # 正常恢复 GPR
```

加了这 8 字节后:40 + 128 + 8(padding) = 176,`call` +8 = 184,`184 ≡ 8 (mod 16)` ✓。handler 入口栈对齐正确,`movaps` 不再炸。

这里有两处必须小心。一是 `leaq 8(%rsp), %rdi`:padding 是临时垫的,传给 C handler 的 `InterruptFrame*` 必须跳过它、仍指向原来的 `r15` 字段,这样 `InterruptFrame` 结构体布局完全不用改。二是恢复时先 `addq $8` 弹掉 padding,再按原顺序 `pop` GPR——顺序不能错。

`ISR_NOERRCODE` 和 `ISR_ERRCODE` 两个宏都改了(异常带错误码的那个,CPU 多压一个错误码,账算下来同样需要这 8 字节 padding 才能满足 ABI)。

**为什么之前从来没炸?** 因为这个 bug 一直在那,只是以前的 IRQ handler 都没让编译器生成 `movaps`。直到这一章给键盘 handler 塞了双路分发、触发了 SSE 优化,才把这个潜伏的对齐问题顶出水面。这也是栈对齐 bug 最阴险的地方:它**静默**——简单 handler 不触发,只有编译器恰好用了对齐敏感的指令才暴露,排查难度高。教训很直接:**ISR stub 必须保证 handler 入口 `RSP ≡ 8 (mod 16)`,这是 ABI 的硬性要求,不是可选项**。

> 顺带一提:修完 #GP 后,链接器还会因为另一个符号报错——`__dso_handle` 未定义。这是因为 030 当时的 `WindowManager::instance()` 里那个 `static WindowManager wm;` 单例**带析构函数**,编译器要把它通过 `__cxa_atexit(func, arg, __dso_handle)` 注册成程序退出时调用的析构。我们的 freestanding 内核没有动态链接,得自己提供这个符号。在 [crt_stub.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/crt_stub.cpp) 里补一个 `void* __dso_handle = nullptr;` 就够了(内核没有 DSO,空指针足矣)。一个对齐 bug 引出一个链接符号,这是「从零搭 GUI」这类大改动典型的连带效应。注:这个 `instance()` 单例是 tag 030 当时的实现,visor 解耦后该单例已随 `WindowManager` 整体外置移除;`crt_stub.cpp` 里的 `__dso_handle = nullptr`(line 115)本身至今仍在。

### 双光标偏移:这不是 bug,是 PS/2 的宿命

**现象。** 在 QEMU VNC 里,屏幕上**同时出现两个光标**:QEMU 自带的圆点,和我们画的那只箭头。两者之间有个**固定偏移**,而且方向随鼠标初始位置变化——初始化到屏幕中央 `(512, 384)`,我们的箭头在圆点的右下方;初始化到 `(0, 0)`,箭头跑到圆点的左上方;偏移量约等于初始坐标。

**根因。** PS/2 协议**只报告相对位移**(dx/dy),不报告绝对位置(见 OSDev Mouse Input)。VNC 客户端那边的宿主光标是用**绝对坐标**渲染的;我们 guest 这边只能从初始位置开始、不断累加 dx/dy 推算光标位置:

```text
宿主: cursor = (absolute_x, absolute_y)           ← VNC 客户端直接知道
我们: cursor = (init_x + Σdx, init_y + Σdy)        ← 只能累积位移
```

两边从不同的起点出发、累积相同的位移,所以**偏移恒等于初始位置的差值**。PS/2 协议(1980 年代设计)根本没有「获取绝对位置」的命令,这不是我们代码的 bug。

**怎么缓解。** 030 的办法是两手:一是 QEMU 配置加 `-usb -device usb-tablet`(见 [qemu.cmake](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/cmake/qemu.cmake)),让 VNC 的宿主光标改用绝对定位渲染,顺手解决鼠标抓取卡住的问题;二是 guest 侧把鼠标初始位置设成 `(0, 0)`,用户从左上角手动移入对齐。两个光标仍然各自独立,但至少可用。

彻底的解法是写一个真正的 USB HID 驱动,读 USB tablet 的绝对坐标(0~width, 0~height)——但那已经超出这一章的范围了,留作长期目标。这条记录的价值在于:遇到「两个光标对不上」时,先别怀疑自己的绘图代码,去查输入协议本身能不能给绝对坐标。

## 验证

和前面几章一样,030 的验证分三层:纯逻辑用 host 单测、机内集成用 QEMU kernel 测试、视觉效果用 `make run` 肉眼看。

**第一层:host 单元测试。** 鼠标包解析、事件队列、窗口命中、窗口管理器的 Z 序/合成/拖拽,这些纯逻辑不碰真硬件,在 host 上 `-O2` 编、用 `CINUX_HOST_TEST` 门控跑。和 014 一样是「镜像」测法——把内核里的逻辑抄一份到测试里(因为带 `io_inb` 内联汇编、PIC 调用的内核代码在 host 上跑不起来),测「给我这个输入,算出来的事件/状态对不对」:

```bash
ctest --test-dir build -R "mouse|event_queue|window|canvas" --output-on-failure
```

覆盖:`test_mouse`(3 字节包解析、9 位符号扩展、Y 轴翻转、边沿检测、clamp)、`test_event_queue`(环形满/空/回卷/丢弃)、`test_window`(构造、ID 自增、标题栏、内容、关闭按钮命中、`contains`、blit)、`test_window_manager`(create/destroy/raise、Z 序、composite、handle_mouse 拖拽)、`test_canvas`(029 已有 + 新的离屏 `init(w,h)`)。一次全跑也行:

```bash
cmake --build build --target test_host
```

**第二层:QEMU kernel 测试。** 真正跑内核代码、走真 IRQ 的机内测,在 `main_test.cpp` 里注册了四个 GUI 测试套:

```bash
cmake --build build --target run-big-kernel-test
```

它会跑 `run_mouse_event_tests`(鼠标事件流:PS/2 包 → EventQueue → MouseEvent)、`run_window_tests`、`run_window_manager_tests`(create/destroy/raise/拖拽的端到端)、`run_gui_integration_tests`(`gui_init` 接线、键盘双路分发、PIT 滴答回调、鼠标事件经 EventQueue 流到窗口管理器)。这是把前面「镜像测」验证过的逻辑,放到真实的内核 + QEMU + PS/2 模拟器里再验一遍整条管线。

> **tag-bound 说明**:`main_test.cpp` 注册这四个套是 030 当时的机内测布局。visor 解耦后,`Window` / `WindowManager` / GUI 集成测试随整个 GUI 外置到 `third_party/Cinux-GUI/test/`,改用 standalone ctest 跑(`test_window.cpp` + `test_window_manager.cpp` 等,不再是 kernel 内 `main_test` 注册的套);`main_test.cpp` 里现存的 GUI 套只剩 `run_mouse_event_tests`(main_test.cpp:105/1198)。本章的机内测叙述按 tag 030 当时布局。

**第三层:视觉效果。** 想亲眼看到三个窗口、亲手拖一下:

```bash
cmake --build build --target run
```

`qemu.cmake` 已经带了 `-usb -device usb-tablet`,所以鼠标抓取和对齐都正常。预期:开机进 GUI 后,屏幕上三个错落的窗口(`Window 1/2/3`),鼠标是一只带黑边的白色箭头;按住左键拖标题栏,窗口跟着走;点窗口把它顶到最前;点右上角红叉关闭。

```text
[GUI] Initialising GUI subsystem...
[GUI] Demo rendered to framebuffer.
...
[GUI] ===== Milestone 030: GUI Window Manager =====
[MOUSE] Mouse enabled (ACK received).
[MOUSE] PS/2 mouse driver initialised.
[GUI] WindowManager initialised with 3 test windows.
[GUI] GUI tick callback registered on PIT.
```

看到 `WindowManager initialised with 3 test windows` 和 `GUI tick callback registered on PIT`,就说明整条输入管线 + 合成循环都起来了。

## 下一站

到 030,我们有了能拖动的窗口,但这些窗口**里面是空的**——内容区就一片浅灰,什么都干不了。键盘事件虽然已经能进事件队列,但 `handle_key()` 还是空的,没人消费。

下一步要解决的自然是:**让窗口里真的能跑东西**。具体说,我们希望键盘事件不再是「进了队列就石沉大海」,而是真正送到当前前台窗口、被它消费——比如在一个窗口里打字,字就出现在那个窗口里。这会把「窗口」从一个会动的矩形,变成一个真正能承载内容的容器。怎么实现,是下一章的事。

## 参考

- OSDev Wiki — Mouse Input(PS/2 鼠标 3 字节包格式、相对位移特性):https://wiki.osdev.org/Mouse_Input
- OSDev Wiki — "8042" PS/2 Controller(初始化命令 `0xA8 / 0x20 / 0x60 / 0xD4 + 0xF4`):https://wiki.osdev.org/%228042%22_PS/2_Controller
- System V Application Binary Interface — AMD64 Architecture Processor Supplement,§3.2.2 The Stack Frame(函数入口 `(%rsp + 8)` 必须是 16 的倍数):https://gitlab.com/x86-psABIs/x86-64-ABI
- QEMU 鼠标光标偏移(VNC 绝对光标 vs PS/2 相对位移):https://torgeir.dev/2024/02/qemu-mouse-cursor-offset/
