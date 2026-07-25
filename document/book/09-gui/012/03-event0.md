---
title: 03 · /dev/event0
---

# /dev/event0

## /dev/event0:input event queue + evdev 角色

### 角色定位:不解码,只桥接

`/dev/event0` mirror 的是 Linux 的 evdev(`/dev/input/event*`)。它**不解码硬件**——解码在 `mouse.cpp`(PS/2 三字节包 → `MouseEvent`)和 `keyboard.cpp`(scancode → `KeyEvent`),014 讲过的不重讲。它干的活只有一件:**在解码完之后双写一行 `push_event`,把内核的 `Event` 桥到用户态 fd**。这是「内核到用户态的 input 投递边界」——evdev 的全部角色。

看 [mouse.cpp](../../../kernel/drivers/mouse/mouse.cpp#L281),鼠标在 `update_absolute` 里 7 个事件分支(move + 3 个 down + 3 个 up,分布在 `281/292/301/310/321/330/339`),每个 `g_event_queue_.enqueue(ev)` 后面跟一行:

```cpp
g_event_queue_.enqueue(ev);
// F-GUI-USERSPACE batch 2: mirror every mouse event into /dev/event0
// for userspace consumption (dual-write with the kernel GUI queue).
cinux::input::InputEventDevice::instance().push_event(ev);
```

键盘走的是另一条路:`gui_init.cpp` 里 `on_key_event` listener 双写,见 [gui_init.cpp](../../../kernel/gui/gui_init.cpp#L29-L43)。

```cpp
void on_key_event(const cinux::drivers::KeyEvent& ev) {
    cinux::gui::Event gui_ev{};
    gui_ev.type_     = ev.pressed ? cinux::gui::EventType::KeyDown : cinux::gui::EventType::KeyUp;
    gui_ev.key.ascii = ev.ascii;
    // ... 字段填充 ...
    cinux::drivers::Mouse::event_queue().enqueue(gui_ev);
    cinux::input::InputEventDevice::instance().push_event(gui_ev);  // ← 双写
}
```

键盘的解码在 `keyboard.cpp`,**一行没动**——`on_key_event` 是个 listener,挂在键盘驱动里,解码完调它。这样 `keyboard.cpp` 不沾 GUI 依赖(CODING-TASTE §14:driver 不依赖上层),listener 是接缝。这是「让驱动保持纯粹」的标准套路。

### 为什么不能复用 GUI 的 SPSC 队列

GUI 那把 `EventQueue` 是 lock-free SPSC——头注释自陈「假设 IRQ handler 唯一生产者 + WM 唯一消费者,靠 `InterruptGuard` 即可,无需额外同步」,见 [event.hpp](../../../kernel/gui/event.hpp#L100-L110)。但 `/dev/event0` 的生产者**不止一个**:

- mouse 的 IRQ12 处理(mouse.cpp)
- keyboard 的 listener(`on_key_event`,被 keyboard ISR 链调)

两个生产者并发往队列里塞,无锁 SPSC 立刻竞态——head/tail 指针的 read-modify-write 没有原子保护,会丢事件、会写花。所以 `InputEventDevice` 自带一把 `Spinlock`:

```cpp
class InputEventDevice {
    cinux::lib::RingBuffer<cinux::gui::Event, kInputEventQueueSize> events_;
    cinux::proc::Spinlock                                           lock_;
    cinux::proc::Task*                                              read_waiters_{nullptr};
};
```

见 [input_event_device.hpp](../../../kernel/drivers/input/input_event_device.hpp#L44-L72)。头注释自陈这个 MPSC 设计决策:

> MPSC safety: there are two producers (mouse IRQ12 + keyboard via the GUI listener), so the lock-free SPSC EventQueue used by the GUI cannot be shared here (it assumes a single consumer as well). This device therefore keeps its own RingBuffer guarded by a Spinlock -- the irq_guard also makes the prepare_to_wait() in read() atomic vs a concurrent push_event(), closing the lost-wakeup window the way PTY/pipe reads do.

这个判据你要带走:**「多个硬中断可能同时入队时,无锁 SPSC 不够,必须上锁或用 MPSC 结构」**。它不是「加锁总比无锁安全」的教条——GUI 的 SPSC 在单生产者前提下确实更快更简单;但 `/dev/event0` 的双生产者假设打破了 SPSC 的前提,上锁是被迫的。

### push_event:ISR 上下文的铁律

入队逻辑见 [input_event_device.cpp](../../../kernel/drivers/input/input_event_device.cpp#L35-L46):

```cpp
void InputEventDevice::push_event(const cinux::gui::Event& ev) {
    auto g = lock_.irq_guard();
    if (!events_.full()) {
        events_.push(ev);  // silent drop on overflow (matches the GUI EventQueue)
    }
    cinux::net::wake_all(read_waiters_);
}
```

三件事,每件都有理由:

1. **`irq_guard`**:同时关中断 + 持 `Spinlock`。生产者(两个 ISR)互斥,跟读者也互斥(下面会看到 read 路径在同一把锁里 `prepare_to_wait`)。
2. **满了静默丢**:`events_.full()` 就 drop,不阻塞、不报错——跟 GUI EventQueue 一致策略。设备事件流不该因为用户态 read 慢就卡住整个中断链。
3. **`wake_all` 只 unblock,不 inline `schedule`**:这是 ISR 铁律。`Scheduler::unblock` 只把 waiter 置 `Ready`、塞回 run-queue,**绝不**在 ISR 里调 `schedule()` 切走——`sti`-in-syscall 会撞上 LAPIC tick,内核栈被 trap 出 #DF,sysret 路径就烂了(071/084 都点过这条纪律)。唤醒是「标记可运行」,真正切走等中断返回时调度器自己来。

### read 路径:三个要点

read 全文见 [input_event_device.cpp](../../../kernel/drivers/input/input_event_device.cpp#L52-L90),三个要点挨个拆:

```cpp
cinux::lib::ErrorOr<int64_t> InputEventDeviceOps::read(const cinux::fs::Inode*, uint64_t,
                                                       void* buf, uint64_t count) {
    if (count < sizeof(cinux::gui::Event)) {
        return cinux::lib::Error::InvalidArgument;  // (1) evdev 语义
    }
    auto& dev = InputEventDevice::instance();
    for (;;) {
        bool need_block = false;
        {
            auto              guard = dev.lock_.irq_guard();
            cinux::gui::Event ev{};
            if (dev.events_.pop(ev)) {
                std::memcpy(buf, &ev, sizeof(ev));  // (3) memcpy,不是 copy_to_user
                return static_cast<int64_t>(sizeof(ev));
            }
            cinux::proc::Task* self = cinux::proc::Scheduler::current();
            if (self == nullptr) {
                return static_cast<int64_t>(0);
            }
            cinux::net::wait_enqueue(dev.read_waiters_, self);
            cinux::proc::Scheduler::prepare_to_wait(self);  // (2) 自标 Blocked,原子
            need_block = true;
        }  // IRQs 恢复 + 锁释放后才切走
        if (need_block) {
            cinux::proc::Scheduler::schedule_blocked();
        }
    }
}
```

**(1) evdev 语义**。`count < sizeof(Event)` 直接 `EINVAL`——mirror Linux `/dev/input/eventN` 的规矩:read 至少得能装下一个完整事件,否则不给半个。`sizeof(cinux::gui::Event)` 是多少要看 `Event` 的字节布局,用户态 struct 必须 mirror(下一节调试现场会展开)。一个数值上的巧合:`cinux::gui::Event` 的 sizeof 跟 Linux `struct input_event` 一样是 24(虽然字段完全不同),所以「最小 read 字节数」门槛数值上对齐——但别因此以为字段能共用。

**(2) 阻塞读闭合 lost-wakeup**。这是 071/084 反复点过的 prepare-to-wait 范式,见 [scheduler.hpp](../../../kernel/proc/scheduler.hpp#L180-L207)。经典「先检查、后阻塞」的死穴:检查队列空 → 释放锁 → 阻塞,这三步之间如果有个生产者 `push_event`+`wake_all` 插进来,wakeup 就丢了,读者永远睡死。解法是「在**同一把锁内**自标 `Blocked`」:

- `prepare_to_wait(self)` 在 `irq_guard` 内,把 self 状态置 `Blocked`——这一步原子,生产者的 `wake_all` 看到的要么是还没睡的 `Ready`(那就直接跳过),要么是已标 `Blocked` 的(那就 unblock 它),不会出现「检查时空、阻塞前被唤醒、再阻塞」的中间态。
- `schedule_blocked()` 在锁**外**调,真正切走。

这一锁双用是 input device 的设计精髓:**同一把 `Spinlock` 既是生产者互斥锁,又是 prepare-to-wait 的 waiter-lock**。少一把锁,少一处竞态。

**(3) `buf` 是 kernel staging,不是 user pointer**。这是 read 路径最隐蔽的坑。`InodeOps::read` 收的 `buf` 是 `sys_read` 提供的**内核 staging buffer**——`sys_read` 之后自己 `copy_to_user` 把 staging 搬到真用户指针(注释里 console TTY / PTY / pipe read 同范式)。所以这里写 staging 必须 `std::memcpy`,**绝不能** `cinux::user::copy_to_user`——`copy_to_user` 的 `is_user_vaddr` 会拒绝 kernel 地址(它就是设计来挡 kernel pointer 的),返 false,read 就返 `Fault`,sys_read 翻成 `-EFAULT`。

对比下面 `/dev/fb0` 的 ioctl:那里 `arg` **是** user pointer(syscall 层透传),所以 `fb_dev::ioctl` 直接 `copy_to_user`。**read 的 buf 和 ioctl 的 arg 性质两样**——这是「教程即验证」最容易踩的坑,下一节调试现场会给真实现场。

### poll 路径:报就绪 + 挂 waiter 原子

poll 见 [input_event_device.cpp](../../../kernel/drivers/input/input_event_device.cpp#L92-L110):

```cpp
uint32_t InputEventDeviceOps::poll_events(const cinux::fs::Inode*, cinux::proc::Task* waiter,
                                          bool* registered) {
    auto&    dev  = InputEventDevice::instance();
    auto     g    = dev.lock_.irq_guard();
    uint32_t mask = 0;
    if (!dev.events_.empty()) {
        mask |= cinux::fs::kPollIn;
    }
    if (waiter != nullptr) {
        cinux::net::wait_enqueue(dev.read_waiters_, waiter);
        if (registered != nullptr) {
            *registered = true;
        }
    }
    return mask;
}
```

全 pipe 范式(084 讲过):「检查队列非空报 `kPollIn`」+「`wait_enqueue` 挂 waiter」**在同一把锁内原子完成**。要是拆开(先检查报 ready,再挂 waiter),中间 `push_event`+`wake_all` 插进来,waiter 还没挂上,wakeup 就丢了——经典 lost-wakeup。同一把锁闭合它,跟 read 路径同款思路。`poll_detach_waiter` 反向摘 waiter,不展开。

诚实说一句:**poll 路径在本机没实跑过**。dev note 明记「poll path 未跑机制测(smoke 只 read);用户态 host 用 poll 时再验」。本章把设计讲透了,「真在 QEMU 里跑过」的章只盖到 `read` 路径——`poll` 等 ring3 GUI host 真用上时再盖章。这是诚实的「只设计、未实跑」。

