---
title: Lab 030 · 窗口管理器:鼠标、事件队列与那条 #GP
---

# Lab 030 · 窗口管理器:鼠标、事件队列与那条 #GP

> 029 给了块「画布」,030 把它变成会动的桌面:接上第一只 PS/2 鼠标、搭一套统一事件队列、做出能拖能关有 Z 序的窗口,再让窗口管理器每帧合成。这个 lab 把它拆成五层来吃:PS/2 三字节包怎么解、统一事件队列凭什么安全、窗口命中怎么算、窗口管理器「画从底到顶 / 点从顶到底」的对称性,最后压一个硬核排错——开机即 `#GP`,根因是一个潜伏已久的栈对齐 bug。没有新代码要写,是理解 + 手算 + 排错演练。

## 实验目标

- 手算 PS/2 三字节包:9 位有符号位移、Y 轴翻转、`bit3` 包同步,给出字节流能推出光标轨迹和产生的事件。
- 说清统一 `EventQueue`(128 容量环形)的三个设计点:满了静默丢、为什么有一个排空点、以及「注释自称 SPSC 却有两个生产站点」凭什么还安全。
- 用窗口的真实几何算命中:`contains` 是否覆盖标题栏、关闭按钮那块 14×14 的确切区域。
- 推断窗口管理器的 Z 序行为:点中谁、谁被 raise、合成时按什么顺序画、为什么拖拽要记 `drag_offset`。
- 独立完成「开机 `#GP`」的排错演练:写出至少三条假设,定位到 System V ABI 栈对齐违反,算清那 8 字节 padding 的账,并解释它为什么**到现在才**炸。

## 前置条件

- 029 通过;理解 PS/2 键盘(014)、PIT(011)、双缓冲画布(029)、heap/VMM。
- 能用 `CINUX_GUI=ON` 构建:`cmake -B build -DCINUX_GUI=ON && cmake --build build`。
- 读完主书 [030 · 窗口管理器](../../book/09-gui/030-gui-wm-basic.md) 的「代码路线」和「调试现场」两节。`#GP` 排错任务的真因和修法都在那两节里,本 lab 只做推理演练,不抄答案。

## 任务分解

### 任务 1:PS/2 三字节包,和那个会咬人的 Y 轴

PS/2 鼠标每次报告一个 3 字节包,格式见主书「鼠标驱动」一节:

```text
byte0:  bit0=左键 bit1=右键 bit2=中键  bit3=恒1(包同步位)
        bit4=X符号(X_SIGN)  bit5=Y符号(Y_SIGN)  bit6/7=溢出
byte1:  X 位移低 8 位
byte2:  Y 位移低 8 位
```

位移是 **9 位有符号数**:低 8 位在 byte1/byte2,第 9 位(符号)在 byte0 的 bit4/bit5。符号扩展手动做——`X_SIGN` 置位就把值减 256。这是最容易写反的地方,尤其 Y 轴。

下面给你五组输入,请手算「这一包之后,光标绝对坐标怎么变、会 enqueue 哪些事件」。设初始光标在 `(200, 200)`,屏幕 1024×768,`prev_buttons_ = 0`:

```text
P1:  b0=0x08  b1=0x05  b2=0x0A      (无按键、无符号位)
P2:  b0=0x18  b1=0xFB  b2=0x00      (X_SIGN 置位)
P3:  b0=0x28  b1=0x00  b2=0x00      (Y_SIGN 置位)
P4:  在 P1 之前先收到一个垃圾字节 0x00
P5:  b0=0x09  b1=0x00  b2=0x00      (左键按下,无位移)
```

要算清楚的点有这么几个。先是 9 位符号扩展:P2 的 `b1=0xFB=251`,`X_SIGN` 置位 → `dx = 251 - 256 = -5`;P3 的 `b2=0x00` 配 `Y_SIGN` → `raw_dy = 0 - 256 = -256`。接着是会咬人的 Y 轴翻转:PS/2 物理上「上移为正」(`raw_dy > 0`),屏幕坐标却「向下为正」,所以累加时 `mouse_y_ -= raw_dy`,事件里的 `dy` 字段再翻成屏幕空间(`me.dy = -raw_dy`,正=向下)——那么 P3 这个 `raw_dy = -256` 会让光标往哪走、`event.dy` 又是多少?别凭直觉,按公式推。然后是包同步:P4 那个 `0x00` 的 `bit3` 是 0,不是合法包首,会被丢掉,直到下一个 `bit3=1` 的字节重新锁住边界,这正是丢了几个字节也能重新对齐的原因。最后是边沿检测:P5 有按键但位移为零,位移为零不产生 `MouseMove`;按键靠 `pressed = new & ~prev` 检测状态翻转,只在「那一刻」产生一个 `MouseDown`,不会每包重复报。

> 对应 host 测:`test/unit/test_mouse.cpp` 里的 `"mouse: negative dx=-256 via X_SIGN"`、`"mouse: negative dy=-256 via Y_SIGN"`、`"mouse: byte0 without ALWAYS_1 flag is discarded"`、`"mouse: same button held across packets -> single MouseDown"`、`"mouse: cursor clamped to screen_width-1"`。你的手算结论可以跟这些用例对齐自检。

### 任务 2:统一事件队列,和一个值得较真的诚实点

`EventQueue` 是 128 容量的环形缓冲,`head_/tail_` 两个游标,生产端是输入 IRQ,消费端是 PIT 滴答回调。回答:

- 满了怎么办?为什么选择「静默丢」而不是阻塞或报错?(提示:生产端在中断上下文里,绝不能阻塞。)
- 为什么要一个统一排空点?鼠标和键盘原本各管各的,窗口管理器为什么不想关心「事件是谁来的」?这个「生产/消费分离」和 014 的键盘 ring buffer 是什么关系?
- 较真点。头文件注释把 `EventQueue` 称作 single-producer / single-consumer。但生产站点明明有两个:IRQ1(键盘双路分发)和 IRQ12(鼠标)。它凭什么还能安全工作?请说清楚:它靠的不是无锁原子操作,而是「生产端和消费端都发生在中断上下文、靠中断处理的串行化」换来的简化。这是个够用的简化,但别把它当成可以随便放宽的硬并发保证——这正是主书「事件队列」一节特意点破的地方。

> 对应 host 测:`test/unit/test_event_queue.cpp` 的 `"event_queue: full buffer drops event"`、`"event_queue: wrap-around fill-drain-refill"`、`"event_queue: FIFO preserved across wrap-around"`。

### 任务 3:窗口双缓冲与命中

`Window` 自带一块**离屏 Canvas**(不挂 framebuffer 的那种,029 新增的 `init(w,h)` 构造),标题栏 + 内容先画在离屏画布上,合成时再 `blit_to` 整块拷到屏幕。回答:

- 为什么要绕一层离屏缓冲?这一章的合成是每帧**全量重绘**(clear 成桌面色再从底到顶 blit)。如果窗口内容直接画在屏幕上,clear 这一下会发生什么?画到一半用户看到什么?(撕裂。)
- `contains` 覆盖标题栏吗?看源码:`contains` 的上界用的是 `total_height() = h_ + TITLE_BAR_HEIGHT`,所以标题栏区域**算在窗口内**。这跟测试 `"window: contains includes title bar area"` 一致——点标题栏要点中窗口,这是拖拽的前提。
- 关闭按钮的确切区域。源码里(以 `CLOSE_BUTTON_SIZE=14`、`TITLE_BAR_HEIGHT=20` 计):

  ```text
  cb_x = x_ + w_ - 14 - 3        // 标题栏右上角,距右边 3px
  cb_y = y_ + (20 - 14) / 2      // = y_ + 3,竖直居中于 20px 标题栏
  命中: cb_x <= mx < cb_x+14  且  cb_y <= my < cb_y+14
  ```

  设一个窗口 `Window("W", 100, 100, 320, 240)`(x=100,y=100,w=320,h=240)。算出关闭按钮的命中矩形,然后判断下面四个点哪个**会**触发关闭、哪个**不会**:`(405,110)`、`(417,110)`、`(100,110)`、`(403,103)`。注意右边界和下边界是**开区间**(exclusive)——这对应测试 `"window: close button hit at bottom-right corner (exclusive)"`。

> 对应 host 测:`test/unit/test_window.cpp` 的 `"window: close button hit at exact top-left corner"`、`"window: contains includes title bar area"`、`"window: blit_to places window at correct screen position"`。

### 任务 4:窗口管理器的 Z 序对称性

`WindowManager` 用一个 `Window* windows_[64]` 数组存窗口,index 0 最底、`count_-1` 最顶。它的核心是一个**对称性**:画的时候从底到顶(后画的盖在先画的上面),点的时候从顶到底(第一个命中的就是用户看到的最上层)。

设窗口管理器里有三个窗口,当前 Z 序从底到顶是 `A(0) / B(1) / C(2)`,三者位置部分重叠。顺着下面这条线回答。

先看一次点击的后果:用户点了一个同时落在 A 和 B 矩形内、但不在 C 内的点,`hit_test` 从 `count_-1` 往下扫,第一个命中的是哪个?(`B`。)为什么必须从顶往下——因为重叠时用户「看到的、想点的」永远是最上面那个。点中 B 会 `raise(B)`,B 挪到数组顶端,新的 Z 序变成 `A(0) / C(1) / B(2)`——注意是「抽出 B、后面的前移、B 放末尾」,不是两两交换。紧接着这一帧 `composite()` 会画出什么:它先 `clear(0x00224466)`(桌面色),再按 index 0→`count_-1` 顺序 blit 可见窗口,最后画光标、`flip()`,所以 B 会画在 C 之上,这正是 raise 的视觉效果。

再追问两个设计动机。为什么要存 `Window*` 而不是值?`Window` 持有动态分配的离屏 Canvas(不可拷贝),Z 序重排只需要挪指针(几次赋值),不用搬对象(那要重建画布);那为什么又是固定 64 上限而不是动态容器?这是内核里的朴素选择,规避动态扩容的内存风险,满了 `create` 就返回 `0`(ID 从 1 起,0 是失败哨兵),对应测试 `"wm: create returns 0 when max windows reached"`。最后,为什么拖拽要记 `drag_offset`?`handle_mouse` 在 MouseDown 命中标题栏时记下 `drag_offset_x_ = ev.x - hit->x()`;如果没记这个偏移、MouseMove 时直接把窗口原点设成鼠标坐标,窗口就会猛地窜一下、左上角跳到鼠标位置——记下「抓在窗口内的相对位置」,移动时用「鼠标坐标 − 偏移」还原,窗口才稳稳跟着走。

> 对应 host 测:`test/unit/test_window_manager.cpp` 的 `"wm: hit test gives top window priority"`、`"wm: composite respects Z-order (top overwrites bottom)"`、`"wm: handle_mouse MouseDown on close button destroys window"`、`"wm: handle_mouse MouseMove while dragging updates position"`。

### 任务 5(排错演练):开机即 `#GP`

这是 030 最硬核的部分,来自 `document/notes/030/gp_fault_stack_alignment.md` 的真实记录。

现象是:`CINUX_GUI=ON` 跑,刚打印 `[GUI] ===== Milestone 030: GUI Window Manager =====`,立刻炸:

```text
==== EXCEPTION: #GP (vector 13) ====
  RIP   = 0xFFFFFFFF81001DBB   CS  = 0x0010
  RSP   = 0xFFFF800008047EF8   ...
```

诡异的是:崩溃点不在鼠标代码里,而在键盘的 IRQ1 handler。我们这一章动的是鼠标,键盘 014 就写好了、一直好好的。`CINUX_GUI=OFF` 基线全过。

请你独立写出**至少三条**排查假设,每条配验证手段,并指出真因。重点要把这笔**栈账**算清楚:

```text
CPU 自动压入(IRQ 无错误码): SS,RSP,RFLAGS,CS,RIP = 5 × 8 = 40 字节
ISR stub 压入:                假错误码 + rax..r15 = 16 × 8 = 128 字节
                                                        合计 168 字节
call handler 压入返回地址:                                8 字节
                                                        合计 176 字节
```

要回答的关键问题:

- `176 ≡ ? (mod 16)`。System V AMD64 ABI 要求进入函数的瞬间 `RSP ≡ 8 (mod 16)`(即 `(RSP+8)` 是 16 的倍数)。176 这个数满足吗?(不满足:176 是 16 的倍数,意味着 handler 入口 `RSP ≡ 0`,差了 8 字节。)
- handler 内部 `push %rbx; sub $0x20,%rsp` 后,落到那条 `movaps %xmm0,(%rsp)` 时地址对不对齐?`movaps` 要求 16 字节对齐,不对齐就 `#GP`。
- 要补多少?上面那笔账算到 **176**(修复前,`≡ 0 (mod 16)`,就是错的);在压完 GPR 后、`call` 前 `push $0` 垫 8 字节 padding,账变成 40 + 128 + 8(padding) + 8(call) = **184**(修复后),`184 ≡ 8 (mod 16)` ✓。这 8 字节从哪进、`InterruptFrame*` 指针为什么要 `leaq 8(%rsp)` 跳过它、恢复时为什么要先 `addq $8` 再 pop GPR——主书「调试现场」有完整修法,这里只要求你算对账、说清为什么。
- 为什么现在才炸?这个 bug 一直在那,为什么 014 写键盘时没炸?(提示:以前的 IRQ handler 没让编译器生成 `movaps`;直到这一章给键盘 handler 塞了 GUI 双路分发、触发了 SSE 优化,才把潜伏的对齐问题顶出来。)为什么是鼠标 `init()` 操作 PS/2 控制器触发了键盘的 IRQ1?(8042 命令的副作用产生虚假 IRQ1。)

<details>
<summary>参考方向(自己先写再看)</summary>

- 假设 A(真因):ISR 栈对齐违反 System V ABI。验证:看 RIP 指向的指令是不是 `movaps`;算上面的栈账,发现 handler 入口 `RSP ≡ 0 (mod 16)` 而非要求的 `≡ 8`。修法是补 8 字节 padding,账算到 184。
- 假设 B:鼠标 init 本身写挂了。验证:但崩溃 RIP 在键盘 handler 而非鼠标代码,且 `CINUX_GUI=OFF` 不炸——说明不是鼠标逻辑错,而是「鼠标 init 的硬件副作用(虚假 IRQ1)引爆了一个一直存在的栈 bug」。先证伪「鼠标代码错」。
- 假设 C:某个新 GUI 对象的析构/链接问题。验证:修完 #GP 后,链接器会接着报 `__dso_handle` 未定义——因为 `WindowManager::instance()` 里那个 `static WindowManager wm;` 单例带析构,要走 `__cxa_atexit(..., __dso_handle)`。freestanding 内核没有 DSO,在 `crt_stub.cpp` 补一个 `void* __dso_handle = nullptr;` 即可。这是一条**连带**的链接问题,不是 #GP 的根因,但常被一起撞到。

核心教训:**ISR stub 必须保证 handler 入口 `RSP ≡ 8 (mod 16)`,这是 ABI 硬性要求,不是可选项**;栈对齐 bug 是静默的,只有编译器恰好生成对齐敏感指令时才暴露。

</details>

> 顺带:030 还有个「不是 bug」的硬件特性——QEMU VNC 里出现两个光标(QEMU 圆点 + 我们画的箭头)且有固定偏移。根因是 PS/2 只报告相对位移、VNC 宿主光标用绝对坐标,两者起点不同。缓解是 `-usb -device usb-tablet` + guest 初始位置设 `(0,0)`。这不需要排错,理解「输入协议给不给绝对坐标」即可,详见主书「双光标偏移」一节。

## 接口约束

- `cinux::drivers::Mouse`(全静态,系统只有一只):`init()`(走 `0xA8/0x20/0x60/0xD4+0xF4`,完后仍需 `PIC::unmask(12)`)、`irq12_handler(frame*)`(读端口 0x60、攒 3 字节包、入队、发 EOI)、`poll(MouseEvent&)`、`x()/y()`、`set_screen_bounds(w,h)`、`event_queue()`(返回全局 GUI 队列引用,挂 Mouse 上是历史命名)。
- `cinux::gui`:`EventType {MouseMove,MouseDown,MouseUp,KeyDown,KeyUp}`;`MouseEvent{x,y,dx,dy(正=向下),buttons,left,right,middle}`;`Event{type_; union{mouse;key;}}`。
- `EventQueue`:`BUF_SIZE=128`;`enqueue`(满则静默丢)、`dequeue(out)`、`empty()`、`clear()`。
- `Window`:`TITLE_BAR_HEIGHT=20`、`CLOSE_BUTTON_SIZE=14`、`TITLE_MAX_LEN=63`、`DEFAULT_WIDTH=320`、`DEFAULT_HEIGHT=240`;`Window(title,x,y,w,h)`(h 不含标题栏)、`draw_title_bar(font)`、`draw_content()`、`blit_to(dst)`、`set_position/set_title`、`is_close_button_hit/contains`、`total_height()=h_+20`、`id()`(静态计数器从 1 起)。拷贝构造 delete。
- `WindowManager`(单例 `instance()`):`MAX_WINDOWS=64`、`DESKTOP_COLOR=0x00224466`;`init(screen*,font*)`、`create(title,w,h)→id`(满返回 0)、`destroy(id)`、`raise(id)`、`composite()`、`handle_mouse(ev)`、`handle_key(ev)`(030 为空 stub,键盘进队列但无人消费——本 tag 的诚实留白)、`focused()`、`window_count()`。命中 `hit_test` 从顶往下。
- CMake:`CINUX_GUI`(默认 ON)门控整个 `kernel/gui/`;`-DCINUX_GUI=OFF` 才关掉,关掉后 `irq_handlers.cpp` 提供 `mouse_irq12_handler` 存根(interrupts.S 无条件引用它)。

## 验证步骤

- **任务 1–4(纯逻辑)**:对应 host 测试全绿——

  ```bash
  ctest --test-dir build -R "mouse|event_queue|window" --output-on-failure
  ```

  你的手算结论(dx/dy、光标坐标、命中、Z 序)逐一跟测试断言对齐。一次全跑:`cmake --build build --target test_host`。

- **任务 5(#GP 排错)**:纸上完成假设链 + 栈账;结论对照主书「调试现场」的 `176 → 184` 账和修法。

- **端到端(机内集成测)**:

  ```bash
  cmake --build build --target run-big-kernel-test
  ```

  跑真内核 + 真 IRQ:`run_mouse_event_tests`、`run_window_tests`、`run_window_manager_tests`、`run_gui_integration_tests`(整条输入管线端到端)。

- **视觉效果**:

  ```bash
  cmake --build build --target run
  ```

  `qemu.cmake` 已带 `-usb -device usb-tablet`。预期:进 GUI 后三个错落窗口(`Window 1/2/3`),鼠标是带黑边白箭头;按住左键拖标题栏窗口跟着走;点窗口顶到最前;点右上角红叉关闭。看到 `WindowManager initialised with 3 test windows` 和 `GUI tick callback registered on PIT` 即整条管线起来了。

- **回归**:`cmake -B build -DCINUX_GUI=OFF && ctest --test-dir build`,非 GUI 基线全过(确认 GUI 改动没碰坏别的)。

## 常见故障

- **鼠标上下移动方向反了**:Y 轴翻转写错。PS/2 物理上移 `raw_dy>0`,屏幕向下为正,累加要 `mouse_y_ -= raw_dy`。写反了「能跑但诡异地别扭」。
- **位移值偏 256**:9 位符号扩展漏了。`X_SIGN/Y_SIGN` 置位时要减 256,别只取低 8 位。
- **包错位 / 光标乱跳**:`bit3` 包同步没做。收到字节时若它本该是包首却无 `bit3`,要丢掉等对齐。
- **鼠标动一次就再也不动**:IRQ12 handler 忘了 `PIC::send_eoi(12)`。下一次中断永远不再投递——PS/2 中断的老规矩。
- **开机即 `#GP` 且 RIP 在键盘 handler**:就是任务 5 的栈对齐。算栈账,补 8 字节 padding,handler 入口要落到 `RSP ≡ 8 (mod 16)`。
- **修完 #GP 后链接报 `__dso_handle` 未定义**:`WindowManager` 单例带析构,走 `__cxa_atexit`。在 `crt_stub.cpp` 补 `void* __dso_handle = nullptr;`。
- **VNC 里两个光标对不上**:不是绘图 bug,是 PS/2 只给相对位移。`-usb -device usb-tablet` 缓解,彻底解法是 USB HID 绝对坐标(超出 030 范围)。
- **`CINUX_GUI=OFF` 基线挂了**:`mouse_irq12_handler` 存根没在非 GUI 构建里提供(interrupts.S 无条件引用它),或 ISR 栈对齐改动影响到了别的向量。

## 通过标准

- 任务 1:给定字节流能算出 9 位有符号 dx/dy、Y 翻转后的光标增量、`event.dy` 的正负、包同步与边沿检测的事件;`test_mouse` 的符号/同步/clamp 用例全绿。
- 任务 2:能说清「满了静默丢」「统一排空点」两个设计,并**较真地**指出 SPSC 注释与「两个生产站点」的事实、解释它靠中断上下文串行化而非无锁原语保证安全。
- 任务 3:能算出给定窗口的关闭按钮命中矩形(含开区间边界)、判断给定点的命中结果,并解释离屏双缓冲为什么避免撕裂。
- 任务 4:给定重叠窗口能推断点中谁、raise 后的 Z 序、合成绘制顺序,并说清「存指针不存值」「记 drag_offset」两个设计动机。
- 任务 5:写出至少三条假设(含「栈对齐」真因),算清 `168 → 176(错) → 184(对)` 这笔账,说清为什么 bug 直到本章才被引爆,以及 `__dso_handle` 这条连带链接问题。
- `CINUX_GUI=ON` 的 `run` 看到三窗口可拖可关;`run-big-kernel-test` 四个 GUI 套全过;`CINUX_GUI=OFF` 基线全过。
- 能口头回答:为什么命中从顶往下而合成从底往顶?为什么 `EventQueue` 有两个生产者还敢叫 SPSC?为什么一个潜伏的栈对齐 bug 会被鼠标初始化引爆?
