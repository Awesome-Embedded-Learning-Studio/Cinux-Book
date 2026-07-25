---
title: 03 · userspace host + 内核薄接缝 + 数据路径
---

# userspace host + 内核薄接缝 + 数据路径

## userspace host 进程:一个普通的 ring3 程序

讲完 core 和表,该看真正驱动桌面的那个进程了。Cinux 的 userspace host 住在 [`user/cinux_gui_host/main.cpp`](../../../user/cinux_gui_host/main.cpp),它就是一个普通的 ring3 ELF——`open`/`mmap`/`read`/`poll`/`fork`/`execve` 全是 syscall,跟 `/bin/sh` 没有本质区别。

它干的活用一句话讲完:**打开 `/dev/fb0` 拿到画布、打开 `/dev/event0` 拿到输入、填一张 Host 表、循环调 `core->pump()`**。分三段看。

**第一段,main 里把设备和 GUI 会话立起来**（[`main.cpp:486`](../../../user/cinux_gui_host/main.cpp#L486) 起):

```cpp
int fb_fd = open("/dev/fb0", O_RDWR);
if (fb_fd < 0) {
    return 1;
}
struct fb_screen_info info;
if (ioctl(fb_fd, FBIOGET_SCREENINFO, &info) != 0) {
    return 2;
}
size_t   sz = static_cast<size_t>(info.pitch) * static_cast<size_t>(info.height);
uint8_t* fb =
    static_cast<uint8_t*>(mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0));
if (fb == reinterpret_cast<uint8_t*>(-1)) {
    return 3;
}
int ev_fd = open("/dev/event0", O_RDONLY);
```

`/dev/fb0` 是内核暴露的帧缓冲设备(087 详讲),`ioctl(FBIOGET_SCREENINFO, ...)` 拿到宽高 pitch,`mmap(... MAP_SHARED ...)` 把它映射进自己的地址空间——往后往这块内存写像素,就是往屏幕写像素。`/dev/event0` 是内核暴露的输入事件设备(也是 087),host 用普通的 `read` 从里面取鼠标键盘事件。这两个 fd 就是 host 和硬件之间**全部**的接缝。

接着建控件树(WindowManager、几个 Window、DesktopIcon、TerminalWidget),然后填表:

```cpp
Host host{};
host.core.flush          = host_flush;
host.core.poll_event     = (ev_fd >= 0) ? host_poll_event : nullptr;
host.core.dispatch_event = (ev_fd >= 0) ? host_dispatch_event : nullptr;
host.core.render_frame   = host_render_frame;
host.core.now_ms         = host_now_ms;
host.core.alloc          = host_alloc;
host.core.free           = host_free;
host.core.log            = host_log;
host.desktop             = nullptr;   /* host 自己用 fork/execve 起 shell,不需要 core 替它 spawn */
host.ctx                 = &st;

auto* core = new GuiCore(&host, info.width, info.height, PixelFormat::kXrgb8888);
```

（[`main.cpp:558`](../../../user/cinux_gui_host/main.cpp#L558)。)看清楚:除了 `desktop` 填 `nullptr`(host 自己会起 shell,用不到 core 替它 spawn),其他每个回调都指向这个文件里实现的 `host_*` 函数。`GuiCore` 构造时拿到了 staging 缓冲的所有权——它会按 `width*height*4` 分配一块内存(看 [`gui_core.cpp:32`](../../../third_party/Cinux-GUI/core/gui_core.cpp#L32) 的 `new uint8_t[...]`),以后每帧 host 就往这块内存上画。

**第二段,主循环**(就这两行,[`main.cpp:573`](../../../user/cinux_gui_host/main.cpp#L573)):

```cpp
unsigned long iter = (n_pump == 0) ? ~0UL : n_pump;
for (unsigned long i = 0; i < iter; ++i) {
    core->pump();
}
```

`argv[1]` 解析成 `n_pump`:`"0"` 表示无限(取 `~0UL`),数字表示跑几圈后退出——后者是个**冒烟自检**:跑完指定圈数后,代码会读屏幕中心那个像素是不是非零([`main.cpp:578`](../../../user/cinux_gui_host/main.cpp#L578) 起),黑屏返回 5。整个桌面就靠这个 `for` 撑着——没有 PIT 回调、没有内核线程、没有 IRQ,就是一个普通进程在用户态反复 pump。

**第三段,看那几个 `host_*` 回调具体干什么**——这才是 host-neutral core 真正落地的地方。

输入侧 `host_poll_event`（[`main.cpp:220`](../../../user/cinux_gui_host/main.cpp#L220)):`poll(ev_fd, timeout=0)` 非阻塞探一下,有数据就 `read` 一个 `kernel_event` 出来,把它的 `type_` 字段翻译成 core 的 `EventCode` + 对应 payload,塞进 `EventHeader` 缓冲:

```cpp
bool host_poll_event(void* ctx, EventHeader* out, uint16_t cap) {
    auto* st = static_cast<HostState*>(ctx);
    if (out == nullptr || st->ev_fd < 0 || cap < sizeof(EventHeader)) {
        return false;
    }
    struct pollfd pfd;
    pfd.fd      = st->ev_fd;
    pfd.events  = POLLIN;
    pfd.revents = 0;
    // Nonblocking (timeout=0): pump stays fully responsive -- mouse/keyboard
    // events are read with no latency.  A 10ms blocking wait was tried (to
    // release CPU during gcc compiles) but reverted: it didn't fix the real
    // stall (NVMe sync poll under NvmeBlockDevice::lock_, can't yield) and
    // only added input lag.  The compile-time stall is a known v1.0.0 issue;
    // the cure is async IO (v1.1+).
    if (poll(&pfd, 1, 0) <= 0) {
        return false;
    }
    kernel_event kev;
    ssize_t      n = read(st->ev_fd, &kev, sizeof(kev));
    if (n != static_cast<ssize_t>(sizeof(kev))) {
        return false;
    }
    out->magic = kEventMagic; out->version = kAbiVersion; out->flags = 0;
    auto* tail = reinterpret_cast<uint8_t*>(out) + sizeof(EventHeader);
    switch (kev.type_) {
    case kMove: case kMouseDown: case kMouseUp: {
        /* 写 payload 前先查 cap,确保 tail 缓冲放得下 */
        if (cap < sizeof(EventHeader) + sizeof(PointerPayload)) { return false; }
        out->type = EventCode::kPointer;
        PointerPayload p;
        p.kind = (kev.type_ == kMouseDown) ? kPointerKindDown
                : (kev.type_ == kMouseUp) ? kPointerKindUp : kPointerKindMove;
        p.x = kev.mouse.x; p.y = kev.mouse.y;
        p.dx = kev.mouse.dx; p.dy = kev.mouse.dy; p.buttons = kev.mouse.buttons;
        memcpy(tail, &p, sizeof(p));
        out->payload_len = sizeof(p);
        break;
    }
    case kKeyDown: case kKeyUp: {
        /* 同样查 cap 后填 KeycodePayload(此处省略) */
        out->type  = EventCode::kKeycode;
        out->flags = (kev.type_ == kKeyDown) ? kEventFlagPressed : 0;
        break;
    }
    default: return false;
    }
    return true;
}
```

注意这一去一回的**序列化**:内核那边把鼠标键盘事件塞进 `/dev/event0` 时用的是自己的 `kernel_event` 结构(就是 [`main.cpp:97`](../../../user/cinux_gui_host/main.cpp#L97) 镜像的 `cinux::gui::Event` 内存布局),host 这边读出来再翻译成 core 的 `EventHeader` + `PointerPayload`/`KeycodePayload`。为什么要绕这一去一回?不是为了性能,而是为了让 `pump()` 真的中性——它只认 `EventHeader`,不认 `cinux::gui::Event`,换了输入源(比如换成 USB HID、换成 SDL 的事件)这行代码不用改,改 `host_poll_event` 一个函数就行。

那行被注释保留的「`A 10ms blocking wait was tried ... but reverted`」也值得读:它是一个真实的**折腾记录**。曾经想让 host 在 poll 上阻塞 10ms,好让 gcc 编译时腾出 CPU;试了发现没用——真正的卡是 NVMe 驱动在 `NvmeBlockDevice::lock_` 上同步 poll 不能 yield,这个阻塞改掉了输入延迟却没解决卡顿,于是回滚。留这条注释,就是给后面接手的人省一次同样的弯路。

显示侧 `host_flush`（[`main.cpp:145`](../../../user/cinux_gui_host/main.cpp#L145)):收到 core 推过来的脏矩形,把 staging 缓冲里对应那块像素 memcpy 进 mmap 来的 framebuffer:

```cpp
void host_flush(void* ctx, int x, int y, int w, int h, const void* pixels, uint32_t stride,
                PixelFormat fmt) {
    auto* st = static_cast<HostState*>(ctx);
    if (st->fb == nullptr || pixels == nullptr || w <= 0 || h <= 0) { return; }
    if (fmt != PixelFormat::kXrgb8888) { return; }
    /* ... clip 到屏幕边界 ... */
    const uint32_t  src_stride_px = stride / 4u;
    const uint32_t  dst_stride_px = st->fb_pitch / 4u;
    const uint32_t* src = static_cast<const uint32_t*>(pixels);
    uint32_t*       dst = reinterpret_cast<uint32_t*>(st->fb);
    for (int row = 0; row < h; ++row) {
        const uint32_t py = static_cast<uint32_t>(y + row);
        const uint32_t* srow =
            src + static_cast<size_t>(py) * src_stride_px + static_cast<uint32_t>(x);
        uint32_t* drow = dst + static_cast<size_t>(py) * dst_stride_px + static_cast<uint32_t>(x);
        for (int p = 0; p < w; ++p) {
            drow[p] = srow[p];
        }
    }
}
```

注意 `src` 用的是 core 拥有的 staging 基地址、`dst` 用的是 mmap 来的 framebuffer——这正是 flush 显示模型在硬件那头的落地:core 不知道 framebuffer 长什么样,host 不知道 staging 怎么分配的,两者只在 `host_flush` 的参数表上交班。

合成侧 `host_render_frame`（[`main.cpp:187`](../../../user/cinux_gui_host/main.cpp#L187)):先把每个活跃 shell 的 PTY 输出 drain 进对应的 `TerminalWidget`,然后调 `desktop.render(staging, font, &dirty)` 把控件树画进 core 拥有的 staging 缓冲,再把 dirty 报告回 `frame->rects`:

```cpp
void host_render_frame(void* ctx, Frame* frame) {
    auto* st = static_cast<HostState*>(ctx);
    if (frame == nullptr) { return; }
    /* drain 每个 shell 的 PTY → TerminalWidget */
    char buf[512];
    for (uint32_t i = 0; i < HostState::kMaxShells; ++i) {
        if (st->shells_[i].master_fd < 0) { continue; }
        ssize_t n;
        while ((n = read(st->shells_[i].master_fd, buf, sizeof(buf))) > 0) {
            st->shells_[i].term.write(buf, static_cast<uint32_t>(n));
        }
    }
    Surface s{frame->pixels, frame->width, frame->height, frame->stride, frame->format};
    Region  dirty;
    st->desktop.render(s, st->font, &dirty);
    /* defensive workaround: core 早年 remove_window 不标脏,host 用每帧全屏 flush
     * 兜底。那个 core bug 已经修掉(remove_window 现在会 invalidate 关窗足迹),
     * 但 host 这条全屏 flush 留着没拆——拆它需要实跑验证关窗路径真不留残影,
     * 本章没做,是已知遗留,见章末「这章没做的」。*/
    if (frame->max_rects >= 1u) {
        frame->rects[0] = Rect{0, 0, static_cast<int32_t>(frame->width),
                               static_cast<int32_t>(frame->height)};
        frame->count    = 1u;
    }
}
```

这一段就是前面说的「已知遗留」:host 现在每帧都报一个全屏脏矩形,把 `pump()` 第 2 步尾部那个 idle 跳过和第 3 步的 Region 收拢实际短路掉了。**这件事的来龙去脉值得说清楚**——它不是「core 还没修」的现况,而是一条**遗留的防御性 workaround**:当年 core 的 `WindowManager::remove_window` 关窗时不标脏,host 只能每帧全屏 flush 兜底(否则关掉的窗口会在屏幕上留残影,直到光标划过那块区域才被覆盖)。但那个 core bug **早已修掉**——现在的 `remove_window` 在 unlink 之前先存下关窗足迹 `stale`,然后 `invalidate(stale)`（[`window_manager.cpp:39`](../../../third_party/Cinux-GUI/core/widget/window_manager.cpp#L39)),连 `add_window`/`add_icon`/光标移动/Z-order 变化也都各自 invalidate。源码注释自己也写:「This was a core bug -- hosts worked around it with a full-screen dirty flush each frame, but the proper fix is invalidating here, mirroring add_window()」。

也就是说:**core 这一侧已经能正确报告脏区了**,host 全屏 flush 在原理上已经不再必要。但 host 那条 `frame->count = 1` 的全屏分支留着没拆,因为拆它需要实跑验证「关一个窗口、看屏幕真不留残影」,这一步本章没做,留到后面(见「这章没做的」)。读者看到这里别以为脏矩形机制没用上:几何代数(Region、半开区间 Rect、`remove_window` 的 `invalidate`)是 core 真的在跑的,只是 host 这一侧目前故意报全屏——把它拆回报真脏区,是个范围明确的收尾活,不是设计缺陷。

> **同一个 main 怎么既能编成 freestanding 友好的 host、又能用 syscall?** 因为它静态链接 musl,syscall 走 musl `libc.a` 的薄包装。看构建脚本 [`tools/musl/build-cinux-gui-host.sh`](../../../tools/musl/build-cinux-gui-host.sh):`g++ -static -nostdlib -no-pie -fno-rtti -fno-exceptions -std=c++17`,链接 21 个 core 源 + `main.cpp` + 一个 [`crt_stub.cpp`](../../../user/cinux_gui_host/crt_stub.cpp)。`-nostdlib` 不链默认运行时,改用 musl 的 `Scrt1.o`/`crti.o`/`crtn.o` + `-lc -lgcc`;core 是 freestanding C++(没有 STL、没有异常、没有 RTTI),所以一个只含 `operator new/delete → malloc/free` 桩 + `__cxa_pure_virtual → abort` 的 [`crt_stub.cpp`](../../../user/cinux_gui_host/crt_stub.cpp) 就够,**不链 libstdc++**。整个 ELF 静态、无动态依赖,扔进 initramfs 内核 `fork+execve` 就能跑。

## 内核只剩两个文件:薄接缝长什么样

讲完 host 进程,回头看内核这边剩了多少。整个 `kernel/gui/` 子目录现在**只有三个文件**:`event.cpp`(一个老的统一事件队列,测试在用,生产输入其实走 `/dev/event0`)、`gui_init.cpp`、`desktop_launch.cpp`。后两个才是真正的「薄接缝」。看 [`kernel/gui/CMakeLists.txt`](../../../kernel/gui/CMakeLists.txt) 怎么描述这件事:

> the in-kernel host adapter (host_cinux.cpp) is deleted -- the widget tree + Host ABI table + GuiCore pump now live entirely in the USERSPACE GUI host (user/cinux_gui_host, fork+execve'd by desktop_launch). kernel/gui/ keeps only the kernel-side plumbing the userspace host can't do:
> - event.cpp : unified mouse+keyboard EventQueue (test_mouse_event exercises it; production input reaches the host via /dev/event0, not this queue)
> - gui_init.cpp : PS/2 mouse + keyboard listener -- dual-writes each decoded key into /dev/event0 for the userspace host
> - desktop_launch.cpp: GUI-side launch_userspace (fork+execve /cinux_gui_host) + handoff_framebuffer_to_gui

——也就是说,**旧的内核 host 适配器 `host_cinux.cpp` 整个删掉了**。它当年填的那张 Host 表、它当年实现的 `cinux_flush`/`cinux_poll_event`/`cinux_render_frame` 那一整套,全挪进了 `user/cinux_gui_host/main.cpp`。内核这边不再碰 Host 表。

### gui_init.cpp:ISR 里把事件推进 /dev/event0

第一个文件,[`gui_init.cpp`](../../../kernel/gui/gui_init.cpp),全文 62 行,只做两件事:初始化 PS/2 鼠标、注册一个键盘 listener。看 `gui_start()`:

```cpp
void gui_start() {
    cinux::lib::kprintf("[GUI] ===== Milestone 033: GUI Desktop (F13-B new core) =====\n");

    cinux::drivers::Mouse::init();
    cinux::drivers::Keyboard::register_key_listener(on_key_event);

    cinux::lib::kprintf(
        "[GUI] Mouse + keyboard listener initialised; desktop driven by the "
        "userspace GUI host (/cinux_gui_host).\n");
}
```

（[`gui_init.cpp:47`](../../../kernel/gui/gui_init.cpp#L47)。)`Mouse::init()` 装好 PS/2 鼠标的中断处理(IRQ12),`Keyboard::register_key_listener(on_key_event)` 给键盘驱动挂一个回调。**键盘驱动本身没有任何 GUI 依赖**——它只负责解码 scancode,然后调「谁注册了 listener 就调谁」,这是 Cinux 的 §14 约定(代码味道:驱动不该硬挂上层)。`on_key_event` 是 `gui_init.cpp` 自己的文件内函数:

```cpp
void on_key_event(const cinux::drivers::KeyEvent& ev) {
    cinux::gui::Event gui_ev{};
    gui_ev.type_     = ev.pressed ? cinux::gui::EventType::KeyDown : cinux::gui::EventType::KeyUp;
    gui_ev.key.ascii = ev.ascii;
    gui_ev.key.scancode = ev.scancode;
    gui_ev.key.pressed  = ev.pressed;
    gui_ev.key.shift    = ev.shift;
    gui_ev.key.ctrl     = ev.ctrl;
    gui_ev.key.alt      = ev.alt;
    cinux::drivers::Mouse::event_queue().enqueue(gui_ev);
    cinux::input::InputEventDevice::instance().push_event(gui_ev);
}
```

（[`gui_init.cpp:29`](../../../kernel/gui/gui_init.cpp#L29)。)注意它把同一个事件塞进**两个队列**:`Mouse::event_queue()` 是老的内核 GUI 队列(现在只有测试还在用),`InputEventDevice::instance().push_event()` 是 `/dev/event0` 的环形缓冲——后者才是 userspace host `read` 的来源。这就是「dual-write」:同一份事件,内核老路径和 userspace 路径各收一份,迁移期内两条路都能跑,迁完可以把老队列拆掉。

鼠标那一侧不用 listener,因为鼠标驱动自己就调 `push_event`。看 [`mouse.cpp`](../../../kernel/drivers/mouse/mouse.cpp#L273):鼠标 IRQ12 的 ISR 在解码完一个数据包后,既 enqueue 进老队列、又 mirror 进 `/dev/event0`。move 分支是这样:

```cpp
// 如果有移动,enqueue 一个 MouseMove 事件
if (ev_dx != 0 || ev_dy != 0) {
    Event ev{};
    ev.type_ = EventType::MouseMove;
    ev.mouse = me;
    g_event_queue_.enqueue(ev);                                  // 老队列(测试用)
    cinux::input::InputEventDevice::instance().push_event(ev);   // /dev/event0(生产用)
}
```

（`move` 分支在 [`mouse.cpp:274`](../../../kernel/drivers/mouse/mouse.cpp#L274) 起,`push_event` 那一行在 [`:281`](../../../kernel/drivers/mouse/mouse.cpp#L281)。)鼠标按键的 down/up 也是同样的 dual-write,每个分支都成对出现([`:289/:292`](../../../kernel/drivers/mouse/mouse.cpp#L289)、[`:298/:301`](../../../kernel/drivers/mouse/mouse.cpp#L298)、…)。

`InputEventDevice` 本身是个 `/dev/event0` 的 InodeOps 适配器,内部一个环形缓冲 + 一把自旋锁。**为什么这里要锁,而老 `EventQueue` 不用?** 因为老队列是 SPSC(单生产者单消费者)——只有鼠标 ISR 推、只有内核 GUI worker 消费。`/dev/event0` 有**两个生产者**(鼠标 IRQ12 + 键盘 listener),所以必须用锁保护的环形缓冲。消费者那一侧,host 进程的 `read`/`poll` 走 syscall 进内核,ops 适配器从环形缓冲里取事件、必要时把 reader 挂起等。

### desktop_launch.cpp:fork+execve 把 host 拉起来

第二个文件,[`desktop_launch.cpp`](../../../kernel/gui/desktop_launch.cpp),全文 74 行,核心是 `launch_userspace()`:

```cpp
void launch_userspace() {
    cinux::lib::kprintf("[INIT] ===== Milestone 035: GUI Desktop (b3b userspace) =====\n");

    // 装 PS/2 鼠标 + 键盘 listener(listener 把每个 KeyEvent mirror 进 /dev/event0)
    cinux::gui::gui_start();

    // fork+execve userspace GUI host。它 open /dev/fb0 + /dev/event0、
    // 建控件树、永远 pump GuiCore。替代旧的内核 gui_worker 线程。
    int child_pid = fork(g_pid_alloc);
    if (child_pid == 0) {
        auto* child        = Scheduler::current();
        child->addr_space  = new cinux::mm::AddressSpace();
        const char* argv[] = {"/cinux_gui_host", "0", nullptr};
        const char* envp[] = {nullptr};
        launch_user_program("/cinux_gui_host", argv, envp);
        Scheduler::exit_current();  // unreachable
    }
    cinux::lib::kprintf("[INIT] userspace GUI host launched (pid=%d)\n", child_pid);
}
```

（[`desktop_launch.cpp:36`](../../../kernel/gui/desktop_launch.cpp#L36)。)这就是「内核拉起桌面」的全部:**先 `gui_start()` 把输入侧装好(否则 host 进程起来读 `/dev/event0` 也读不到东西),再 `fork` 一个新 task、给它一个全新的地址空间、`launch_user_program` 加载 `/cinux_gui_host` 这个 ELF 并跳进 ring3**。`launch_user_program` 是 035 讲过的那个「execve + 建用户栈 + 跳用户态」共享函数(永不返回,失败 `exit_current` 兜底);argv `"0"` 是告诉 host「无限 pump」(看上面 host main 的 `iter` 解析)。fork 完父进程(就是 `kernel_init`)打印一行 pid 就返回了——桌面从此是个独立的 userspace 进程,内核继续往下走自己的 init 流程。

文件里还有个 `handoff_framebuffer_to_gui`（[`desktop_launch.cpp:60`](../../../kernel/gui/desktop_launch.cpp#L60)),它现在的活儿非常薄:**什么 GUI 结构都不建**(host 进程自己建),只是把文本控制台 detach 掉,让例行 kprintf 不再覆盖 framebuffer:

```cpp
void handoff_framebuffer_to_gui(cinux::drivers::Framebuffer& fb, cinux::drivers::PSFFont& font,
                                cinux::drivers::Console& console) {
    (void)fb;
    (void)font;
    cinux::lib::kprintf_set_sink_enabled(cinux::drivers::Console::console_sink_adapter, &console,
                                         false);
}
```

`fb`/`font` 形参都 `(void)` 掉了——它们留在接口上是为了 §14 的接口对称(非 GUI 那边的对应函数签名得对齐),但函数体不用。`fb` 的初始化和 `set_system_framebuffer` 在更早的 `kernel_main` 里就跑过了,所以 `/dev/fb0` 的 mmap 在 host 进程里能正确解析到 VBE framebuffer 的物理内存。`kpanic` 重新启用控制台 sink,所以崩了还是能看到堆栈——这条 detach 是单向的运行期开关,不是配置。

### file gate:GUI 启动 vs 非 GUI 启动

`launch_userspace()` 这个函数有**两份实现**:`kernel/gui/desktop_launch.cpp`(GUI 版,fork+execve `/cinux_gui_host`)和 `kernel/proc/shell_launch.cpp`(非 GUI 版,fork+execve `/bin/sh`)。CMake 按开关选编一份:

```cmake
# §14: shell_launch.cpp is the non-GUI userspace entry (fork + exec /bin/sh).
# GUI builds link kernel/gui/desktop_launch.cpp instead (the gui/ subdirectory is
# added only under if(CINUX_GUI) in kernel/CMakeLists.txt).  Exactly one of the
# two launch_userspace() impls is linked.
if(NOT CINUX_GUI)
    target_sources(big_kernel_common PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/shell_launch.cpp
    )
endif()
```

（[`kernel/proc/CMakeLists.txt:37`](../../../kernel/proc/CMakeLists.txt#L37)。)调用方永远是一句 `cinux::proc::launch_userspace(...)`,零 `#ifdef`——链接器在链接期自动解析到被选中的那份。这叫 **file gate**(文件级开关),它和源码级 `#ifdef` 的区别是:file gate 切的是「编哪个文件」,源码读起来是一条直线;`#ifdef` 切的是「读哪段」,源码读起来要分叉。下一卷会专门讲这套收尾。

## 整条数据路径:从鼠标挪一格到屏幕像素变

把上面所有零件串起来,看一次完整的「鼠标挪一格,屏幕上光标跟着动」走的是哪条路:

1. **鼠标硬件**产生中断,IRQ12 进内核 ISR(`mouse_irq12_handler`)。
2. ISR 解码数据包,算出新光标位置 + button 状态,构造一个 `cinux::gui::Event{type_=MouseMove, mouse={x,y,dx,dy,buttons}}`。
3. ISR 把这个 event **dual-write**:一份进老的 `Mouse::event_queue()`(测试用),一份 `InputEventDevice::instance().push_event()` 进 `/dev/event0` 的环形缓冲（[`mouse.cpp:281`](../../../kernel/drivers/mouse/mouse.cpp#L281))。
4. 与此同时,userspace 的 `/cinux_gui_host` 进程正在它的 `for(;;) core->pump()` 循环里。`pump()` 第 1 步调 `host_poll_event`,后者 `poll(ev_fd, 0)` 探到 `/dev/event0` 有数据,`read` 一个 `kernel_event` 出来,翻译成 `EventHeader{type=kPointer} + PointerPayload{kind=Move, x, y}`。
5. `pump()` 把这个事件交给 `host_dispatch_event`,后者 `wm.process_pointer(p)`——WindowManager 更新光标位置、标光标足迹脏(`invalidate(old + new footprint)`)。
6. `pump()` 第 2 步调 `host_render_frame`:`desktop.render(staging, font, &dirty)` 把控件树 + 新光标画进 core 拥有的 staging 缓冲,报告脏矩形(目前 host 故意报全屏)。
7. `pump()` 第 3 步把脏矩形过一遍 `Region`(去重 / 坍缩)。
8. `pump()` 第 4 步对每个 region 矩形调 `host_flush`,后者把 staging 里对应那块像素 memcpy 进 mmap 的 framebuffer——**屏幕像素变了**。

整条路径里,**内核只做了第 1-3 步**(ISR + push_event),**第 4-8 步全在 userspace**。鼠标解码在内核(因为 PS/2 是中断驱动的硬件协议),光栅化/合成/写 fb 全在用户态。这就是「薄接缝」的含义:内核只负责「把硬件事件变成可读的字节流」,剩下全是用户态进程的事。

