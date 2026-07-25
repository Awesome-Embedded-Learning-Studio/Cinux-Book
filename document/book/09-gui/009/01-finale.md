---
title: 01 · GUI 解耦收尾:把源码里的 #ifdef 全赶到 CMake 那边去
---

# GUI 解耦收尾:把源码里的 #ifdef 全赶到 CMake 那边去

> 054 把 GUI 的核心拔成了一个 host-neutral 的库,但留了一笔账没收:源码里还散着几处 `#ifdef CINUX_GUI` / `#ifdef CINUX_USB`,读 `init.cpp` 读到一半会「分叉成两条路」。那一章把它们标成边界,说「等 xHCI 落地再收」。现在 xHCI(055)有了,夹在中间还顺手修了一个 SMP 迁移竞态(053)——这笔账该还了。这一章就做这件事,原则只有一句:**开关归 CMake,源码里不写 `#ifdef`**。做法叫 file gate:同一个接口写两份实现文件,CMake 按开关选编一份,调用处永远是一条直线。B 档:验证不靠新功能,靠 `grep` 证明 `init.cpp` / `main.cpp` / `irq_handlers.cpp` 三个文件里 `#ifdef CINUX_*` 归零,再靠四种构建组合(GUI/USB 开关各两档)全都链接通过——关掉 GUI 或 USB 时,靠一组空壳文件顶上,链接器照旧解析得了符号。
>
> 一个诚实的小提醒:这一章里把启动逻辑抽成函数的那一步,正是 053 里那个「看起来人畜无害的重构踩出每次必现 panic」的重构本身。这一章讲它**为什么该抽**;053 讲它**抽完踩出的坑**。两章是同一件事的两面。

## 这章咱们要点亮什么

1. **§14 铁律**:为什么「开关写在源码的 `#ifdef` 里」是一种坏味道——它让代码「读到一半分叉」,而正确的姿势是「全编进去也读得通」。
2. **file gate 正解**:同一个接口、两份实现文件、CMake 选编一份、调用处零 `#ifdef`。这是 `#ifdef` 的替代品,不是另一种 `#ifdef`。
3. **USB 空壳**:把 `xHCIController` 藏进一个自由函数,再给一份同名空实现,调用方既不用 `#include` 控制器头,也不用在调用点判编译开关。
4. **启动二选一收敛**:`init.cpp` 里那段「GUI 启桌面 / 非 GUI fork shell」的分叉,怎么收成一句调用——以及它为什么是这套收尾里最值得讲的一个。
5. **USB 双 gate**:为什么 USB 不能一个开关管到底,得拆成「核心传输」和「HID 注入」两层,各自一个 gate。

## §14 铁律:开关归 CMake,源码零 #ifdef

先说清楚要守的规矩。Cinux 有一条写进 CODING-TASTE 的约定(叫 §14),一句话:**编译开关归 CMake 管,源码里不写 `#ifdef`**。

这条规矩针对的是一种很常见的写法。内核里有些能力是可选的——GUI 可编可不编、USB 可编可不编。最省事的办法是在调用处写:

```cpp
#ifdef CINUX_USB
    usb::init();
#endif
```

看起来人畜无害,但它有个读起来很别扭的性质:**这段代码你不能顺着读下去**。读到 `#ifdef` 那行,你得先在脑子里分叉成两条路——「编了 USB,走这边;没编,走那边」——再把两条路各自读完。条件一多,源文件就成了「读到一半就裂开」的样子。§14 的判据很直白:一份源码,**全编进去也该读得通**;靠 `#ifdef` 在读的人脑子里切分路径,就是违规。

那开关去哪?去 CMake。CMake 本来就在决定「哪些文件进编译」,让它在「编哪份实现」上做选择,是它的本职。于是正解形态是:**同一个接口,写两份实现文件,CMake 按开关选编一份**。调用处既不 `#include` 条件头,也不写 `#ifdef`,就是一句普通调用;链接器在链接期自动解析到被选中的那份实现。这叫 **file gate**(文件级开关),它和源码级 `#ifdef` 的区别是:file gate 切的是「编哪个文件」,源码读起来是一条直线;`#ifdef` 切的是「读哪段」,源码读起来要分叉。

这一章干的就是把 054 留下的几处源码 `#ifdef` 全换成 file gate。先看改之前有多散:在 GUI 解耦前的 fork 点上,三个核心文件里一共 **7 处** `#ifdef CINUX_*`——`init.cpp` 3 处、`main.cpp` 3 处、`irq_handlers.cpp` 1 处。改完之后,这三个文件里 `#ifdef CINUX_GUI` / `#ifdef CINUX_USB` 的计数**全是 0**。下面挨个看怎么收。

## file gate 长什么样:先看一个最小的

在动 USB 之前,先看这套写法最干净的形态,在 `proc/CMakeLists.txt` 里:

```cmake
# §14: shell_launch.cpp is the non-GUI userspace entry (fork + exec /bin/sh).
# GUI builds link kernel/gui/desktop_launch.cpp instead (the gui/ subdirectory is
# added only under if(CINUX_GUI) in kernel/CMakeLists.txt).  Exactly one of the
# two launch_userspace() impls is linked.
if(NOT CINUX_GUI)
    target_sources(big_kernel_common PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/shell_launch.cpp)
endif()
```

非 GUI 构建编 `shell_launch.cpp`,GUI 构建不编它(改编 `kernel/gui/desktop_launch.cpp`,那个子目录只在 `if(CINUX_GUI)` 下加进编译)。两个文件各写一份 `launch_userspace()`,CMake 保证**恰好一份**被链接。调用方(`init.cpp`)那头是什么样,后面专门讲——先记住这个「两份实现、CMake 选一份」的骨架,后面几处都是它的变体。

## USB:把控制器藏起来,留个同名空壳

第一处要收的是 USB。`init.cpp` 启动时要调一句 USB 初始化,GUI 的 worker 线程每帧还要轮询一次 USB 事件环。在 file gate 之前,这两处都是 `#ifdef CINUX_USB` 守着、直接调 `XHCIController` 的静态方法。这有两个毛病:一是调用方得 `#include` 控制器那个重头文件,二是调用点挂着 `#ifdef`。

收的办法分两步。第一步,把「轮询事件环」这件事收成一个自由函数 `usb::poll_input()`,放在 USB 模块自己里头:

```cpp
void poll_input() {
    // Dequeue transfer events off the event ring -> device listeners (tablet /
    // keyboard) decode + inject + re-arm.  No-op until init() enumerated a
    // controller. ...
    if (XHCIController::has_controller()) {
        XHCIController::instance().poll_events();
    }
}
```

（`usb_init.cpp:159`。）这一下把 `XHCIController` 的静态门面(`has_controller` / `instance`)全藏进了 `.cpp`。调用方只见 `usb::poll_input()` 一个名字,既不用 `include` 控制器头,调用点也变成一行普通调用。封装的意义不在整洁——它是后面 file gate 能成立的前提:调用方依赖的是一个**稳定的接口名**,不是某个只有 USB 编了才存在的类。

第二步,给这个接口配一份空壳。新建 `usb_stub.cpp`,里头是两个同名空函数:

```cpp
// usb_stub.cpp —— 当 CINUX_USB 关闭(或 HID 层不编)时的空操作桩
void init() {
    // USB compiled out -- nothing to bring up.
}
void poll_input() {
    // USB compiled out -- no event ring to service.
}
```

（`usb_stub.cpp:19` 起,文件头 `:5` 的注释把机制说得很明白。)然后 CMake 来选:`usb_init.cpp`(真实现)和 `usb_stub.cpp`(空壳)只编一份:

```cmake
if(CINUX_USB AND CINUX_GUI)
    target_sources(... usb/usb_init.cpp ...)
endif()
...
if(NOT (CINUX_USB AND CINUX_GUI))   # 见下文,这里是 NOT (USB AND GUI)
    target_sources(... usb/usb_stub.cpp)
endif()
```

于是 `init.cpp` 那头就干净了——无条件 `#include` 接口头、无条件调 `usb::init()`(`init.cpp:58`),`#ifdef` 一个不剩:

```cpp
// usb_init.hpp 是无条件包含:当 CINUX_USB 关闭时,usb_stub.cpp 提供
// 空的 usb::init()/poll_input() (§14 file gate),因此此 TU 不需要任何 #ifdef。
#include "kernel/drivers/usb/usb_init.hpp"
...
    cinux::drivers::usb::init();
```

（`init.cpp:22`-`:23` 的注释和无条件 include、`:58` 的调用。)注意那个 stub 的触发条件是 `NOT (CINUX_USB AND CINUX_GUI)`,**不是** `NOT CINUX_USB`——为什么是这么个绕的条件,留到最后一节「USB 双 gate」讲,那是这套收尾里最微妙的一处。

## 启动二选一:读到一半分叉的头号反例

前面几处是热身。这套收尾真正想治的「头号反例」,在 `init.cpp` 的 `kernel_init_thread` 里。内核启动到最后要把控制权交到用户态,这一步在 GUI 解耦前是这么写的(意译):

```cpp
void kernel_init_thread() {
    ...
#ifdef CINUX_GUI
    // GUI:起桌面、注册鼠标/窗口管理器、起一个 worker 线程刷新屏幕
    gui_start();
    spawn gui_worker;
#else
    // 非 GUI:fork 一个 shell,用老的 sys_read/sys_write
    fork + execve("/bin/sh");
#endif
}
```

这是 §14 最不想看到的样子:一段读到一半、`#ifdef` 把它劈成两股完全不同的逻辑。两股逻辑还都不短,读到 `#else` 得在脑子里把前面那段整个按下、换一套上下文接着读。这个函数在 fork 点有 91 行,大半是这两股分叉。

治法跟前面一样:把「交到用户态」这件事抽成一个接口 `launch_userspace()`,两份实现,CMake 选编一份。抽完之后,`kernel_init_thread` 里这一整段塌成了一句:

```cpp
    // Bring up userspace.  GUI build: desktop + gui_worker thread
    // (kernel/gui/desktop_launch.cpp).  Non-GUI build: fork + exec /bin/sh
    // (kernel/proc/shell_launch.cpp).  §14: one interface, two impl files,
    // CMake selects which to link -- no #ifdef here.
    launch_userspace();
```

（`init.cpp:51`。）函数从 91 行瘦到 34 行,`#ifdef` 全没了。两份实现各有各的去处:GUI 那份在 `desktop_launch.cpp`,非 GUI 那份在 `shell_launch.cpp`,接口在 `userspace.hpp` 里无条件声明(`userspace.hpp:29`)。CMake 那头前面已经看过——`if(NOT CINUX_GUI)` 编 `shell_launch.cpp`,GUI 那份走 `gui/` 子目录。

两份实现各自干什么,读一眼就懂。GUI 这份(`desktop_launch.cpp:52`)起桌面、再 `TaskBuilder` 起一个 `gui_worker` 线程;那个线程的循环体,正是 054 讲过的 pump + yield,中间夹一句刚才封装好的 `usb::poll_input()`:

```cpp
void gui_worker_thread() {
    while (true) {
        cinux::gui::pump(&cinux::gui::cinux_host());
        cinux::drivers::usb::poll_input();
        cinux::proc::Scheduler::yield();
    }
}
```

（`desktop_launch.cpp:33`。)非 GUI 这份(`shell_launch.cpp:24`)简单得多:`fork`,子进程建个新地址空间,然后 `launch_user_program("/bin/sh", ...)` 一跳——这个 `launch_user_program` 是 054 里已经收敛好的「建栈 + 跳用户态」共享函数,这里直接复用,不重写第二遍。

> **这一步抽函数,正是 053 那个 SMP bug 的触发点。** 053 讲过一个故事:把这段内联的启动逻辑抽成 `launch_userspace()`、把 `gui_worker` 挪进自己的 TU 之后,双核 `-smp 2` 从「偶发 panic」变成了「每次必 panic」。元凶不是抽出来的代码写错了——单核跑 931 个测试全过——而是「多了一个任务 + 双核并发」本身踩进了一个一直潜伏着的迁移竞态窗口(旧核存上下文、新核取同一份上下文,并发读写写花)。内联版不是没这个 bug,是时序恰好绕开了它。这一章讲的是「这个抽函数为什么该做」(因为它治好了 §14 的头号反例);053 讲的是「抽完踩出的坑怎么填」(给任务加 `on_cpu` 标记,跳过正在被别的核存上下文的任务)。两章合起来才是一个完整的故事:重构是对的,重构踩出来的潜伏 bug 也得一并治了。

## main.cpp 和 irq:把剩下的 #ifdef 收掉

`init.cpp` 是大头,另外两处 `#ifdef` 收起来套路一样,快讲。

`main.cpp` 里 Step 15b 要把帧缓冲和文本控制台交给 GUI(起 Canvas、起窗口管理器、把控制台从 kprintf sink 上摘下来,省得日常日志盖在桌面上)。这步在 GUI 编了/没编时行为完全不同,老写法是 `main.cpp` 里一个 `#ifdef CINUX_GUI` 块。同样抽成接口 `handoff_framebuffer_to_gui(fb, font, console)`,两份实现:GUI 那份在 `desktop_launch.cpp:69`,干刚才说的那些活;非 GUI 那份在 `shell_launch.cpp:45`,是个空壳(参数都 unnamed,函数体就一句注释「GUI compiled out -- nothing to hand off」)。`main.cpp` 那头塌成一句:

```cpp
    // Step 15b: hand the framebuffer + console off to the GUI ... No-op when
    // GUI is compiled out (§14 stub linked).  kpanic re-enables all sinks,
    // so a crash still reaches the screen.
    cinux::proc::handoff_framebuffer_to_gui(fb, font, console);
```

（`main.cpp:180`。)注意那个「kpanic re-enables all sinks」的细节:平时把控制台摘了是为了不挡桌面,但崩溃时 panic handler 会把所有 sink 重新打开,所以就算摘了控制台,崩溃栈照样能打到屏幕上——这是个容易看漏的容错设计。

最后是 `irq_handlers.cpp`。到这次收尾前,它里头有两个 `#ifndef`(注意是 ifndef,反的)守着的空壳 handler:鼠标的 `mouse_irq12_handler`(`#ifndef CINUX_GUI`,fork 点就有)和 xHCI 的 `xhci_irq_handler`(`#ifndef CINUX_USB`,055 引入 xHCI 驱动时照着 mouse 的样子加的)。意思是「GUI/USB 没编时,在这里给个空实现,免得汇编那头的中断桩找不到符号」。收法还是 file gate:把这两个空壳各挪一个独立文件(`mouse_stub.cpp`、`usb_xhci_stub.cpp`),CMake 在对应开关关掉时编它们,`irq_handlers.cpp` 里那两段 `#ifndef` 整块删掉。

为什么要挪成独立文件,而不是留在 `irq_handlers.cpp` 里继续 `#ifndef`?因为 `irq_handlers.cpp` 是**无条件编译**的核心文件——只要它里面还残留一个 `#ifndef`,§14 那个「三个核心文件 `#ifdef` 归零」的硬指标就过不去(`grep` 一抓一个准)。file gate 的判据是「调用处所在的文件零条件编译」,所以空壳和真实现必须各占一个独立的编译单元,由 CMake 在文件级二选一,而不是挤在同一个文件里靠预处理器切。

这里有个细节值得点一下:这两个 stub 都必须是 `extern "C"`。原因是它们是被**汇编**调用的——`interrupts.S` 里的 `ISR_IRQ irq12_stub, mouse_irq12_handler, 12`(`interrupts.S:381`)用 C 调用约定 `call` 这个符号。C++ 不加 `extern "C"` 会把名字 mangle 成 `_Z21mouse_irq12_handlerP...`,汇编里查的是裸名,链接就断了。所以 stub 的函数体虽然空,签名上的 `extern "C"` 是硬需求,不是风格:

```cpp
extern "C" void mouse_irq12_handler(cinux::arch::InterruptFrame* /*frame*/) {
    // EOI is owned by the ISR stub (irq12_stub in interrupts.S).
}
```

（`mouse_stub.cpp:16`,`usb_xhci_stub.cpp:15` 同理。)顺带一句:函数体是空的、连 EOI 都不发,因为 `ISR_IRQ` 宏在 handler 返回后自己会发 EOI——stub 里再发一遍就成双 EOI 了。这两个被汇编调用的 stub 都是这套。

## USB 双 gate:为什么不能一个开关管到底

最后一处,也是这套收尾里最该慢慢讲的。前面 USB 的 stub 条件是 `NOT (CINUX_USB AND CINUX_GUI)`,不是 `NOT CINUX_USB`——为什么?

因为 USB 内部分两层,对 GUI 的依赖不一样。一层是**核心传输**:xHCI 控制器、TRB 环、slot、MSI-X。这层只管「把 USB 包收上来 / 发下去」,不认识鼠标也不认识键盘,所以它**不依赖 GUI**。另一层是 **HID 注入**:`usb_init` 拿到包之后,要解码成鼠标/键盘事件,塞进 `Mouse` / `Keyboard` 队列——而 `Mouse` / `Keyboard` 是 GUI 才编的。于是 HID 这层**依赖 GUI**。

如果还像最开始那样一个 `CINUX_USB` 管到底,就会在「开 USB、关 GUI」这个组合上断链:HID 编了(因为 USB 开),但它调的 `Mouse::set_usb_primary` 所在的 `Mouse` 没编(因为 GUI 关),链接器找不到符号。所以 USB 得拆成两个 gate:

```cmake
# 核心传输:xHCI controller/ring/slot/irq + MSI-X。用 TransferListener 基类,
# 不依赖 Mouse/Keyboard,所以只看 CINUX_USB。
if(CINUX_USB)
    target_sources(... pci/msix.cpp pci/msix_controller.cpp usb/xhci_irq.cpp
                       usb/xhci_controller.cpp usb/xhci_ring.cpp usb/xhci_slot.cpp ...)
endif()

# HID 注入:usb_init + usb_mouse/tablet/keyboard。依赖 Mouse/Keyboard(GUI-gated),
# 所以要 USB AND GUI 同时开。
if(CINUX_USB AND CINUX_GUI)
    target_sources(... usb/usb_init.cpp mouse/usb_mouse.cpp
                       mouse/usb_tablet.cpp keyboard/usb_keyboard.cpp)
endif()

# usb::init()/poll_input() 的空壳:HID 没编(要么 USB 关,要么 GUI 关)时顶上,
# 让 init.cpp 的调用点零 #ifdef 也能链接。
if(NOT (CINUX_USB AND CINUX_GUI))
    target_sources(... usb/usb_stub.cpp)
endif()
```

（`drivers/CMakeLists.txt:40 / :51 / :64`。)这下那个绕的 stub 条件就说得通了:`NOT (USB AND GUI)` 恰好是「HID 没编」的所有情况——要么 USB 整个关了,要么 USB 开但 GUI 关(HID 没编)。这两种情况下 `usb_init.cpp` 都不在,得靠 `usb_stub.cpp` 提供 `usb::init` / `poll_input` 符号。要是图省事写成 `NOT CINUX_USB`,「开 USB 关 GUI」时就没人提供这俩符号,链接断。

这里头有个解耦成败的关键细节:核心传输层之所以能独立于 GUI,是因为它面向 HID 暴露的是一个**基类** `TransferListener`(`xhci_controller.hpp:42`,一个纯虚接口,HID 设备的「包到了」回调),核心只认这个基类指针,不 `#include` 任何鼠标/键盘头。依赖的方向是**单向**的:HID 设备(`UsbTablet`、`UsbKeyboard`)继承 `TransferListener` 去依赖核心,核心绝不反向依赖 HID。要是反过来,在核心里 `#include mouse.hpp`,GUI 耦合就漏进核心传输层,这个双 gate 当场失效——核心再也离不开 GUI 了。

这套双 gate 落地之后,四种构建组合全能链接通过:GUI+USB(默认,跑得最全)、GUI 关 USB 关(全靠 stub)、GUI 关 USB 开(核心传输编、HID 不编、`usb::init` 是空壳)。最后这个组合有个看着别扭、但故意如此的地方:USB 核心传输编进了内核,可 `usb::init` 是个空函数,从不调 `XHCIController::init`/`start`,所以控制器**永远不会被 bring up**。这是 CMake 注释里写明的设计取舍:编了但不用,是无害的死代码;真要抠,可以让核心传输也跟着 HID 一起 gate,但那样「USB 传输层独立于 GUI」这个性质就没了。两害相权,留着这点死代码。

## 诚实的边界

像 054 / 053 那样,把没收干净的摊子说清楚。

**`big_kernel_test` 在非 USB 下还是链接不过。** 这是套测试的债,不是这套收尾的债:`test_xhci.cpp` 无条件进了 `big_kernel_test` 的源列表,又没挂 `#ifdef CINUX_USB` 守卫,非 USB 构建时它引用的 `XHCIController` 等符号找不到。这是 GUI 解耦之前就破的,§14 的 file gate 只管**生产代码**的非 USB 兼容(默认的 `big_kernel` 在非 USB 下已经链接通过);测试那头的 gate 是另一条线,留后续。

**`test/` 目录里还有约 40+ 处 `#ifdef CINUX_HOST_TEST`。** 这不违反 §14。§14 管的是**内核运行源码**里功能开关(开不开 GUI、开不开 USB)的调用处可读性,不约束测试基础设施。测试文件里挂的是 `CINUX_HOST_TEST`——宿主单测开关(如 `test/unit/test_window.cpp:15`),它选编的是「只在 host 单测里才跑的断言」,跟 GUI/USB 这种内核功能开关不是一个层级的东西。同理,`CINUX_LOCKDEP` 是 opt-in 的调试开关(顶层 CMakeLists 用 `option(...)` 声明),别拿 §14 的尺子去量它。

**顺带补了 054 lab 里提到的那笔 host 单测债。** 更早 VFS 把 `read`/`write` 改成返回 `ErrorOr` 之后,调用方得用 `.value()` 解包,但有四个 host 单测的调用点漏改了,全量 `cmake --build build`(含 host 单测)从那时起一直编不过——054 的 lab 里因此专门写了句「别用 ALL 验证,host 单测有既有债」。这四个点现在补齐了,全量构建是绿的,那句提醒也用不着了。

验证该看到什么,见配套 lab。下一章(056)转到安全卷——开 NX/SMEP/SMAP 和 ASLR,那是另一条线了。
