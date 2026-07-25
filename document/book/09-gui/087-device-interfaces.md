---
title: 087 · 让用户态拿到事件、画到屏幕:/dev/event0 与 /dev/fb0 的设备接口
---

# 087 · 让用户态拿到事件、画到屏幕:/dev/event0 与 /dev/fb0 的设备接口

> 做完这章,你在 ring3 里 `open("/dev/event0")` 就能 `read` 出鼠标键盘事件、`open("/dev/fb0")` 再 `mmap` 就能往屏幕直接写像素——GUI host 终于不必只能跑在内核里。一前一后两条接口,前者把内核已经解码好的输入事件搬到用户态,后者把内核已经映射好的显存绑进用户 VMA。
>
> 这一章真正讲的不是「设备节点本身」——那个 064 已经立过了(`InodeOps` 子类 + `add_node` 投影到 `/dev`)。这一章讲的是把**两类已经存在的设备能力**暴露给 ring3 的两条不同形态:`/dev/event0` 是「事件流」,靠 `read/poll` 把内核 input event 排到用户态;`/dev/fb0` 是「内存窗口」,靠 `mmap` 把 VBE 物理显存绑进用户进程。两个设备都只是 `InodeOps` 基类的两套不同 override 子集——064 立的节点机制一行没动,本章给它补两个真设备 inode 子类。
>
> A 档的边界先说在前头:`/dev/event0` 是 flat 节点(不是 Linux 的 `/dev/input/event0`,DevFS 不为 input 子树投影层级——PTY 走的是 `dynamic_lookup` 投 `/dev/pts/<N>`,但 input 子树没有对应的 resolver,要扩 follow-up);`Event` 结构是 Cinux 自己的 `cinux::gui::Event`,不是 Linux 的 `input_event`,用户态要 mirror 它的字节布局(字段顺序和类型,不是 packed);`poll` 是 level-triggered 不是 edge;事件队列满了静默丢(对齐 GUI EventQueue 策略);NVMe 不放这一章(077 讲过 NVMe 驱动)。还有一条验证上的诚实:`poll` 路径在 dev note 里明说「只设计、未实跑」——本章的设计讲透了,但「真在 QEMU 里跑过」只能盖章到 `read` 路径,`poll` 等 ring3 GUI host 真用上时再盖。

## 这章咱们要点亮什么

1. **用户态事件接口 `/dev/event0`**——为什么不能复用 GUI 那把 lock-free SPSC `EventQueue`(双生产者打破单生产者假设),为什么得自带 `RingBuffer` + `Spinlock`,为什么同一把锁还顺手兼任 `prepare_to_wait` 的 waiter-lock(一锁双用:串行化生产者 + 闭合 lost-wakeup)。
2. **用户态显存接口 `/dev/fb0`**——`mmap` 钩子**只回物理地址、不建 PTE**,真正建映射是 page-fault 的活(demand paging 应用到设备内存);为什么必须 uncached(`FLAG_PCD`)——显存不是 RAM,cache 它会让 GUI 写像素只动 cache line 没刷到显存。
3. **MMIO 范式 `mmio32_read/write`**——`volatile uint32` deref + byte-offset,是上面这两类设备(以及 LAPIC/HPET 寄存器)共用的底层纪律;`byte-offset` 跟硬件文档一字对齐,`32-bit` 锁死访问宽度是 QEMU 丢 64-bit MMIO 写逼出来的。
4. **同一个 `InodeOps` 基类派生行为天差地别的子类**——`InputEventDevOps` 只 override `read/poll`(事件流),`FramebufferDevOps` 只 override `mmap/ioctl`(显存窗口);没 override 的 `write`/`create`/`mkdir` 默认 `NotImplemented`,被 sys\_\* 翻译成 `-ENOTTY`/`-EINVAL`。「`/dev/fb0` 没有 read 语义」不是写了个空 read,而是压根没 override——基类替你拒掉了。

## 为什么现在需要它

回到这一刻为止系统有什么。064 立了 DevFS 节点机制(`InodeOps` 子类 + `add_node`),把 `/dev/null`/`zero`/`console` 挂到了 `/dev`,但那几个节点是「内核里有个 sink,读写触发 sink 动作」——设备能力全在内核里。013 把底层 VBE framebuffer 点亮了(bootloader 给的 `PhysBasePtr` + 2MB 大页映射进内核高半区 `KMEM_FB_BASE`),`Framebuffer::put_pixel` 在内核态直接画屏幕。014 把 PS/2 scancode 解析做完了,`mouse.cpp` 三字节包解成 `MouseEvent`、`keyboard.cpp` scancode 翻成 `KeyEvent`。029-033 这卷前面几章,GUI 画布、窗口管理器、原生应用、位图图标、桌面,全在内核态用这块 fb + 这套 input 画窗口。

可这一整套有个尴尬:GUI host 只能跑在**内核态**。要是想让 ring3 进程也能画屏幕、读事件——也就是说,把 GUI host 搬到用户态做成一个普通进程——内核得开两扇门:一扇让用户态读输入事件,一扇让用户态写显存。这一章就是开这两扇门。

源码侧已经齐了:`kernel/drivers/input/input_event_device.{cpp,hpp}` 是事件门,`kernel/drivers/video/fb_dev.{cpp,hpp}` 是显存门,`kernel/drivers/mmio.hpp` 是它们底下那层设备寄存器访问助手。这一章的活不是「写代码」,是**核实**这两扇门端到端真能用——用户态真能 `read/poll /dev/event0` 拿到事件、真能 `mmap /dev/fb0` 画像素。教程即验证。

## 设计图:两条数据流 + 一个底层范式

一眼看穿整章的框图——三条线同框,你能看到 `input` 流和 `fb` 流是两种完全不同的设备接口形态,但底下都踩在同一套 MMIO 纪律上。

```text
                       ┌─────────── 用户态 ring3 ───────────┐
   GUI host            │  open("/dev/event0") read/poll      │   open("/dev/fb0")
                       │  拿鼠标键盘 Event                    │   mmap → 拿到一段虚拟地址
                       └──────────┬───────────────┬──────────┘
                                   │               │
                  ┌─── (A) 事件流 ─▼───┐  ┌─── (B) 显存窗口 ─▼─────────────┐
                  │ InputEventDevice    │  │ FramebufferDevOps::mmap        │
                  │ RingBuffer<Event,   │  │   只回 fb->phys_base()+offset  │
                  │   128> + Spinlock   │  │   (不建 PTE! 只回物理地址)     │
                  │                     │  │                                │
                  │ push_event(ev)      │  │ sys_mmap 给 VMA 打 IoPhys      │
                  │   [irq_guard]       │  │   + 存 vma->phys_base          │
                  │   wake_all          │  │                                │
                  │     (unblock only)  │  │ 用户态首次写触发 #PF           │
                  │                     │  │   page_fault IoPhys 分支:      │
                  │ read/poll 排干      │  │   map_nolock + FLAG_PCD        │
                  └──────────┬──────────┘  │   (uncached, 不调 PMM)         │
                             │             └────────────┬───────────────────┘
                             │                          │
                  ┌──────────▼──────────┐    ┌──────────▼──────────┐
                  │ mouse.cpp 双写 7 处 │    │ Framebuffer::init   │
                  │ keyboard listener   │    │  存 phys_base_      │
                  │   双写(gui_init)   │    │  (VBE PhysBasePtr)  │
                  └─────────────────────┘    └─────────────────────┘

   ───────── (C) MMIO 底层范式,横在所有设备寄存器访问下面 ─────────
   volatile uint32 deref + byte-offset + FLAG_PCD uncached
       ▼                            ▼                              ▼
   LAPIC 寄存器                  HPET 寄存器                    framebuffer 显存
   (0xFEE00000, KMEM_MMIO+0x10000) (0xFED00000, +0x60000)       (VBE PhysBasePtr)
   同一套纪律:设备内存不能 cache,访问宽度锁 32-bit,偏移跟手册对齐
```

(A) 是「事件流」:硬中断进来,内核解码完(`mouse.cpp` 解 PS/2 三字节包、`keyboard.cpp` 解 scancode,014 讲过的不重讲),`push_event` 把 `Event` 塞进 `RingBuffer`(`Spinlock` 守护,因为有两个生产者——不能复用 GUI 那把 lock-free SPSC 队列),用户态 `read/poll` 排干。

(B) 是「内存窗口」:`mmap` 钩子**只回物理地址**,真正建 PTE 是用户态首次写触发 `#PF` 时由 `page_fault.cpp` 的 IoPhys 分支做的——带 `FLAG_PCD`(uncached)。**mmap 是登记意图,fault 才兑现**。

(C) 是横在底下的 MMIO 纪律:`volatile uint32 deref + byte-offset + uncached`,LAPIC 寄存器、HPET 寄存器、framebuffer 显存访问,共用同一套规矩——设备内存不能 cache。这是后面所有「为什么这么设计」的视觉锚。

## MMIO 范式:`mmio32_read/write` 的 volatile + byte-offset

先把底层那块搬出来。MMIO(Memory-Mapped I/O)说的是:把设备寄存器映射到一段物理地址区间(HPET 在 `0xFED00000`、LAPIC 在 `0xFEE00000`),CPU 用普通的 `mov` 读写它,不走 `in`/`out` 端口指令——后那是另一套 PIO 通道,见 [io.hpp](../../../kernel/arch/x86_64/io.hpp#L31-L45) 的 `io_inb`/`io_outb`,跟本章无关。MMIO 的好处是寻址空间大、能用普通内存指令批量操作;代价是访问纪律完全两样。

`mmio.hpp` 全文就十行,但每个字符都有理由:

```cpp
inline uint32_t mmio32_read(volatile uint32_t* base, uint32_t off) {
    return *reinterpret_cast<volatile uint32_t*>(
        reinterpret_cast<uintptr_t>(base) + off);
}

inline void mmio32_write(volatile uint32_t* base, uint32_t off, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(
        reinterpret_cast<uintptr_t>(base) + off) = value;
}
```

见 [mmio.hpp](../../../kernel/drivers/mmio.hpp#L18-L24)。两个设计点焊在签名里:

**(1) `volatile`**。编译器对普通 RAM 会做一堆优化——合并相邻写、消除「读了没用」的读、把「写后立刻读」优化成只读寄存器里的旧值。对 RAM 这些都是好事,对设备寄存器全是灾难。比如写 LAPIC 的 EOI 寄存器(中断结束信号)被编译器合并/消除掉,中断永远不结束,CPU 再也收不到下一个 IRQ。所以 MMIO 访问必须 `volatile`——每次 deref 真的去访问设备,不许缓存、不许合并。这个 `volatile uint32_t* base` 焊在签名上,谁也别想漏。

**(2) byte-offset,不是 element-index**。硬件文档(HPET 规范、Intel SDM LAPIC 章节)全用**字节偏移**记寄存器位置——HPET General Config 在 `0x010`、LAPIC EOI 在 `0x0B0`、LAPIC Timer 在 `0x320`。看 [local_apic.hpp](../../../kernel/drivers/apic/local_apic.hpp#L26-L36) 的常量:

```cpp
constexpr uint32_t kRegEoi          = 0x0B0;  ///< write 0 to end interrupt
constexpr uint32_t kRegLvtTimer     = 0x320;
constexpr uint32_t kRegTimerInit    = 0x380;
```

代码里 `read(kRegEoi)` 跟手册一字对齐。要是用元素下标 `base[0x30]`,C 指针算术对 `uint32_t*` 自动 `×4`,你每次都得心算「`0x0B0 / 4 = 0x2C`」——心算错一个寄存器就飞了。helper 内部 `reinterpret_cast<uintptr_t>(base) + off` 做的是字节加法,绕开了 `uint32_t*` 自动 `×4` 的隐式放大。LAPIC 的薄包装就这么一行,见 [local_apic.cpp](../../../kernel/drivers/apic/local_apic.cpp#L37-L43):

```cpp
uint32_t LocalAPIC::read(uint32_t off) const {
    return mmio32_read(base_, off);
}
void LocalAPIC::write(uint32_t off, uint32_t value) {
    mmio32_write(base_, off, value);
}
```

**为什么名字带 `32`,锁死访问宽度**。这是 QEMU(和部分真机)实机教训出来的——dev note 记了两个坑,本章把坑的**技术**讲清楚(批号那些不进教程):

- **坑之一:General Config 在 `0x010` 不在 `0x008`。** HPET 通用寄存器**间距 `0x10`**,不是密集布局。第一版按 `0x008` 写 ENABLE 位,死活不生效——读回恒 0。根因:`0x008` 落在 reserved 区,写全丢。改成 `0x010` 立刻通。诊断的权威是 QEMU 源码 `hw/timer/hpet.c` 的 `HPET_CFG` 宏,跟 [hpet.hpp](../../../kernel/drivers/hpet/hpet.hpp#L31-L34) 的常量一致:

  ```cpp
  constexpr uint32_t kHpetRegGeneralCaps = 0x000;  // [31:0]=vendor/rev, [63:32]=period(fs)
  constexpr uint32_t kHpetRegGeneralConfig =
      0x010;  // bit 0 = ENABLE_CNF (general regs are 0x10 apart)
  ```

  诊断时有个不对称的线索值得记:对 `0x000` 的**读**是对的(能拿到 `0x9896808086A201`,高 32 位 `0x00989680` 正是 100MHz 周期 fs 值),只有对 `0x008` 的**写**丢——这个「读对写错」正是定位偏移错的方向标。这个坑是 byte-offset 范式被实战逼出来的证据:偏移跟手册对齐不是审美,是「写错位置寄存器就不响应」的硬约束。

- **坑之二:一律 32-bit,别用 64-bit 写。** QEMU **丢 64-bit MMIO 写**——64-bit 写 Config 读回 0;但 32-bit 写 Main Counter(`0x0F0`)能写进 `0xDEADBEEF` 并读回。所以 HPET 读 64-bit 主计数器是「两次 32-bit 半读 + rollover 保护」,见 [hpet.cpp](../../../kernel/drivers/hpet/hpet.cpp#L49-L60):

  ```cpp
  uint64_t HPET::read_counter() const {
      uint32_t high1 = read32(kHpetRegMainCounter + 4);
      uint32_t low   = read32(kHpetRegMainCounter);
      uint32_t high2 = read32(kHpetRegMainCounter + 4);
      if (high1 != high2) {
          low = read32(kHpetRegMainCounter);  // 高半变了,低半重读
      }
      return (static_cast<uint64_t>(high2) << 32) | low;
  }
  ```

  高半两次读之间 low 可能 rollover,如果 high 变了就重读 low,标准无 race 算法。helper 名字带 `32` 就是把这个教训焊死。

最后是 `FLAG_PCD` 映射纪律——所有 MMIO 窗口映射都带 `FLAG_PCD`(Page Cache Disable),uncached。LAPIC 窗口在 [local_apic.cpp](../../../kernel/drivers/apic/local_apic.cpp#L23-L34) 映射时就带了:

```cpp
constexpr uint64_t kLapicMmioVirt = cinux::arch::KMEM_MMIO_BASE + 0x10000;
constexpr uint64_t kFlags =
    cinux::arch::FLAG_PRESENT | cinux::arch::FLAG_WRITABLE | cinux::arch::FLAG_PCD;
if (!cinux::mm::g_vmm.map(kLapicMmioVirt, mmio_phys, kFlags)) {
    return false;
}
```

理由同一类:cache 命中会给设备「假数据」或丢写。013 讲过 `map_mmio` 的恒等映射,这里不重讲页表细节,只点纪律——**volatile 管编译器优化,uncached 管 CPU cache,两条加一起才把 MMIO 访问钉住**。

诚实边界:这套 helper 是「共享范式的提取」,实际只有 LocalAPIC 和 HPET 经它走。别的 MMIO 设备(e1000、virtio、xHCI)各自内联同款 `volatile` deref——**范式一致,未收编**。IOAPIC 甚至走的是反面教材,见 [io_apic.cpp](../../../kernel/drivers/apic/io_apic.cpp#L34-L42):

```cpp
uint32_t IOAPIC::read(uint32_t reg) {
    base_[kIoapicIORegSel / 4] = reg;   // ← 元素下标 base_[常量/4],不是 byte-offset
    return base_[kIoapicIOWin / 4];
}
```

IOAPIC 这里的 `base_[kIoapicIORegSel / 4]`、`base_[kIoapicIOWin / 4]` 是两个**写死的常量偏移**(`kIoapicIORegSel`、`kIoapicIOWin` 是固定 byte offset,运行时 `/4` 折成元素下标),跟 helper 的 byte-offset 范式相反。这不算 bug(IOAPIC 寄存器是「先写选择口再读数据口」的 indirect 寻址,跟直接 MMIO 不一样),但风格上没对齐——是「范式还没收编所有驱动」的诚实留白。

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

## /dev/fb0:framebuffer mmap + IoPhys uncached + ioctl

### 设备形态分野:内存式 vs 流式

先点一个对照:`/dev/event0` 是「**流式设备**」(靠 `read/poll`,事件一条条流过去),`/dev/fb0` 是「**内存式设备**」(靠 `mmap`,把显存整个窗口暴露给用户态直接读写)。所以 `FramebufferDevOps` **只 override `mmap + ioctl`**——`read/write` 走 `InodeOps` 默认 `NotImplemented`,基类替你拒掉(下一节展开)。看 [fb_dev.hpp](../../../kernel/drivers/video/fb_dev.hpp#L23-L29):

```cpp
class FramebufferDevOps : public cinux::fs::InodeOps {
public:
    cinux::lib::ErrorOr<uint64_t> mmap(const cinux::fs::Inode* inode, uint64_t offset,
                                       uint64_t length) override;
    cinux::lib::ErrorOr<int64_t>  ioctl(const cinux::fs::Inode* inode, uint32_t request,
                                        uint64_t arg) override;
};
```

`read`/`write`/`create`/`mkdir` 一个都没 override。这是 VFS 多态的标准玩法:设备多样性 = override 子集不同。

### mmap 钩子:只回物理地址,不建 PTE

`mmap` 全文见 [fb_dev.cpp](../../../kernel/drivers/video/fb_dev.cpp#L35-L47):

```cpp
cinux::lib::ErrorOr<uint64_t> FramebufferDevOps::mmap(const cinux::fs::Inode*, uint64_t offset,
                                                      uint64_t length) {
    Framebuffer* fb = system_framebuffer();
    if (fb == nullptr) {
        return cinux::lib::Error::NotImplemented;  // no framebuffer initialised
    }
    if (offset > fb->size() || length > fb->size() - offset) {
        return cinux::lib::Error::InvalidArgument;
    }
    return fb->phys_base() + offset;
}
```

三件事:

1. `system_framebuffer()` 拿单例,没有就 `NotImplemented`(没初始化显存就别让 mmap)。
2. **越界检查防溢出**:先比 `offset > fb->size()`,再用 `fb->size() - offset` 比 `length`——顺序不能反,否则 `fb->size() - offset` 下溢成天文数字,`length > 天文数字` 永远 false,检查形同虚设。这是「先比再减」的无符号算术铁律。
3. **`return fb->phys_base() + offset`**——这是全章的 punchline 之一:**mmap 钩子只返物理地址,不建 PTE**。

`phys_base()` 是 `Framebuffer` 暴露的 VBE PhysBasePtr,见 [framebuffer.hpp](../../../kernel/drivers/video/framebuffer.hpp#L98):`uint64_t phys_base() const { return phys_base_; }`。这个字段是 `Framebuffer::init` 时存的,见 [framebuffer.cpp](../../../kernel/drivers/video/framebuffer.cpp#L17-L23):

```cpp
void Framebuffer::init(const BootInfo& bi) {
    uint64_t fb_phys = bi.fb_addr;
    phys_base_       = fb_phys;  // F-GUI-USERSPACE b1: expose to /dev/fb0 mmap
    // ...
}
```

bootloader 给的 VBE 物理地址,`init` 存进 `phys_base_`,本章消费这个字段。013 讲过 `init` 里那个 2MB 大页映射进 `KMEM_FB_BASE`,这里不重讲页表——只点「`phys_base_` 是 013 留的字段,本章消费」。

### sys_mmap:登记意图(IoPhys VMA)

真正建映射不是 mmap 钩子的活,是 `sys_mmap` + page-fault 的活。看 [sys_mmap.cpp](../../../kernel/syscall/sys_mmap.cpp#L131-L143) 的 device probe:

```cpp
bool     device_mmap = false;
uint64_t device_phys = 0;
if (backing_inode != nullptr && backing_inode->ops != nullptr) {
    auto mp = backing_inode->ops->mmap(backing_inode, offset, aligned_len);
    if (mp.ok()) {
        device_mmap = true;
        device_phys = mp.value();
    } else if (mp.error() != cinux::lib::Error::NotImplemented) {
        return -to_errno(mp.error());
    }
}
```

`NotImplemented` 当成「这不是设备 mmap」的信号——普通文件/匿名映射走下面那条 page-cache 路径。命中了就把物理地址存下来,给 VMA 打 `IoPhys` 标志,见 [sys_mmap.cpp](../../../kernel/syscall/sys_mmap.cpp#L181-L183):

```cpp
if (device_mmap) {
    vflags |= cinux::mm::VmaFlags::IoPhys;
}
```

然后在 VMA 落定之后,把物理地址写进 `vma->phys_base`,见 [sys_mmap.cpp](../../../kernel/syscall/sys_mmap.cpp#L191-L196):

```cpp
if (device_mmap) {
    // Bind the VMA to device memory.  No inode ref: device memory is
    // not page cache and the VMA is not file-backed in the PageCache
    // sense; phys_base is the per-page physical source for the fault
    // handler.  backing/file_offset stay null/0.
    v->phys_base = device_phys;
}
```

注意此刻用户态那段虚拟地址**还没 PTE**——demand paging。`IoPhys` 标志的定义见 [vma.hpp](../../../kernel/mm/vma.hpp#L63-L68):

```cpp
/// F-GUI-USERSPACE batch 1: device / I-O physical mapping (e.g. framebuffer
/// mmap).  The VMA is pre-bound to a fixed physical range (phys_base); the
/// page-fault handler maps those pages verbatim with FLAG_PCD (uncached)
/// instead of allocating from the PMM.  Teardown (munmap) and fork never
/// free or CoW them -- device memory is not PMM-managed.
IoPhys    = 1 << 7,
```

`IoPhys` 这个标志就是「这不是 RAM 页,别动」的唯一标识——下面的 page-fault handler 看到它就跳过 PMM,直接把物理页原样映射进去;`munmap`/`fork` 看到它也跳过 free/CoW(设备内存不归 PMM 管)。

### page-fault:IoPhys 分支落 uncached PTE

用户态拿到 mmap 返回的虚拟地址,首次写像素时触发 `#PF`——这时候才真正建 PTE。page-fault handler 的 IoPhys 分支见 [page_fault.cpp](../../../kernel/arch/x86_64/page_fault.cpp#L256-L283):

```cpp
} else if (vma != nullptr &&
           cinux::mm::has_flag(vma->flags, cinux::mm::VmaFlags::IoPhys)) {
    const uint64_t io_phys = vma->phys_base + (virt_page - vma->start);
    uint64_t       ioflags =
        cinux::arch::FLAG_PRESENT | cinux::arch::FLAG_USER | cinux::arch::FLAG_PCD;
    if (cinux::mm::has_flag(vma->flags, cinux::mm::VmaFlags::Write)) {
        ioflags |= cinux::arch::FLAG_WRITABLE;
    }
    if (!cinux::mm::has_flag(vma->flags, cinux::mm::VmaFlags::Exec)) {
        ioflags |= cinux::arch::FLAG_NX;
    }
    uint64_t cur_cr3 = cinux::arch::read_cr3();
    if (g_vmm.map_nolock(virt_page, io_phys, ioflags, &cur_cr3)) {
        return;
    }
    return;
}
```

四个要点:

1. **物理地址算出来**:`io_phys = phys_base + (virt_page - vma->start)`,VMA 起始偏移 + fault 页在 VMA 内的偏移。
2. **flags 带 `FLAG_PCD`(uncached)**——这是 MMIO 铁律的「内存映射版本」,跟 LocalAPIC/HPET 寄存器同一类:显存是设备内存不是 RAM,CPU cache 它会让 GUI 写像素只动 cache line 没刷到显存,屏幕不更新。
3. **`FLAG_USER` + `FLAG_WRITABLE` + `FLAG_NX`**——ring3 可写、不可执行(显存不需要执行权限,W^X)。
4. **不调 PMM、不进 page cache、不 `pte_count_inc`**——设备内存不是 PMM 管的,分配/释放都绕开。`map_nolock` 直接落 PTE(用当前 CR3,因为 fault 时已经持有 address space 锁,`map_nolock` 是无锁版本)。

**读代码别在 mmap 钩子里找建表逻辑**——要去 `page_fault.cpp` 找。这是 demand paging 应用到设备内存: mmap 是登记意图, fault 才兑现。

### ioctl:FBIOGET_SCREENINFO 拿屏几何

光有 mmap 不够——用户态 GUI host 还得知道屏幕多大、一行多少字节,才能 size 自己的 canvas。这就是 `ioctl(FBIOGET_SCREENINFO)` 的活,见 [fb_dev.cpp](../../../kernel/drivers/video/fb_dev.cpp#L49-L67):

```cpp
cinux::lib::ErrorOr<int64_t> FramebufferDevOps::ioctl(const cinux::fs::Inode*, uint32_t request,
                                                      uint64_t arg) {
    if (request != kFbioGetScreenInfo) {
        return cinux::lib::Error::NotImplemented;  // sys_ioctl maps this to -ENOTTY
    }
    Framebuffer* fb = system_framebuffer();
    if (fb == nullptr) {
        return cinux::lib::Error::NotImplemented;
    }
    FbScreenInfo info{};
    info.width  = fb->width();
    info.height = fb->height();
    info.pitch  = fb->pitch();
    info.bpp    = 32;  // VBE mode 0x144 is 32-bpp XRGB; Framebuffer assumes it
    if (!cinux::user::copy_to_user(reinterpret_cast<void*>(arg), &info, sizeof(info))) {
        return cinux::lib::Error::Fault;
    }
    return 0;
}
```

回填结构是简化的 4 字段,见 [fb_dev.cpp](../../../kernel/drivers/video/fb_dev.cpp#L27-L32):

```cpp
struct FbScreenInfo {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;  // bytes per scan line
    uint32_t bpp;    // bits per pixel
};
```

三件事得说清:

1. **故意不照搬 Linux `fb_var_screeninfo`**——那个 ~160 字节几十个字段(`xoffset`、`yoffset`、`red.length`、`green.offset`、`transp`、`pixclock`、`hsync_len`…),Cinux 不建模这一堆。只暴露 GUI host 真正消费的 4 个字段:width、height、pitch、bpp。够用就好。
2. **`bpp` 写死 32**——VBE mode `0x144` 是 32-bpp XRGB(8 bit × 4 通道),`Framebuffer` 全假设这个格式,不暴露给用户改。
3. **`kFbioGetScreenInfo = 0x4600`** 是 Cinux 自定义常量,见 [fb_dev.hpp](../../../kernel/drivers/video/fb_dev.hpp#L34-L37)。注释明说「Mirrors Linux FBIOGET_VSCREENINFO in role, not in struct layout」——**数值碰巧和 Linux `FBIOGET_VSCREENINFO` 一致,但 struct 布局完全不同**,用户态别照搬 Linux 用户态结构体,得 mirror 这 4 个字段。

注意 ioctl 的 `arg` **是 user pointer**(syscall 层透传,不像 read 的 staging),所以这里**直接** `copy_to_user(arg, &info, sizeof(info))`,失败返 `Fault`。这跟 read 路径的 staging buf 性质两样——下一节调试现场会对比。

非该 request 一律 `NotImplemented`,被 sys_ioctl 翻译成 `-ENOTTY`(Linux 对「这个 inode 不处理这个 ioctl」的约定)。

## 设备 InodeOps 差异化:同一基类,两套 override

把前面两节收拢成一条设计主线。同一个 `InodeOps` 基类,见 [inode.hpp](../../../kernel/fs/inode.hpp#L77-L116) 的虚函数表(`read`/`write`/`mmap`/`ioctl`/`poll_events`/`create`/`mkdir`/…),`InputEventDevOps` 和 `FramebufferDevOps` 各 override 一个**不同子集**:

```cpp
// input_event_device.hpp:81-88
class InputEventDeviceOps : public cinux::fs::InodeOps {
public:
    cinux::lib::ErrorOr<int64_t> read(...) override;
    uint32_t poll_events(...) override;
    void     poll_detach_waiter(...) override;
};

// fb_dev.hpp:23-29
class FramebufferDevOps : public cinux::fs::InodeOps {
public:
    cinux::lib::ErrorOr<uint64_t> mmap(...) override;
    cinux::lib::ErrorOr<int64_t>  ioctl(...) override;
};
```

没 override 的 `write`/`create`/`mkdir`/`stat`/… 走基类默认 `NotImplemented`,见 [inode.cpp](../../../kernel/fs/inode.cpp#L46-L59):

```cpp
cinux::lib::ErrorOr<int64_t> InodeOps::ioctl(const Inode*, uint32_t, uint64_t) {
    // "This inode type does not implement ioctls."  sys_ioctl translates this
    // into -ENOTTY for the caller ...
    return cinux::lib::Error::NotImplemented;
}

cinux::lib::ErrorOr<uint64_t> InodeOps::mmap(const Inode*, uint64_t, uint64_t) {
    return cinux::lib::Error::NotImplemented;
}
```

所以「`/dev/fb0` 没有 read 语义」**不是写了个空 read 返 0 或返 EINVAL,而是压根没 override**——基类替你拒掉了。这是面向对象 VFS 的标准玩法:**设备多样性 = override 子集不同**,加新设备不动基类,只给它写新子类。064 那里 NullDevOps/ZeroDevOps/ConsoleDevOps 也是这个套路——本章是给 DevFS 再加两个**真设备** inode 子类,不重讲 add_node 机制。

### DevFS 投影:跟 064 同一张 nodes_[] 表

两个设备节点怎么挂上 `/dev`?跟 064 的 null/zero/console 同一个 `add_node` 机制,见 [devfs_init.cpp](../../../kernel/fs/devfs/devfs_init.cpp#L161-L172):

```cpp
// PTY: /dev/ptmx is a cloning device (open() allocates a pair); /dev/pts/<N>
// resolves dynamically to the matching slave inode.  Registered here so
// devfs.cpp itself stays PTY-free (host-testable).
g_devfs.add_node("ptmx", &cinux::drivers::ptmx_ops());
// F-GUI-USERSPACE batch 1: /dev/fb0 -- mmap binds a user VMA to the VBE
// framebuffer physical memory.
g_devfs.add_node("fb0", &cinux::drivers::framebuffer_dev_ops());
// F-GUI-USERSPACE batch 2: /dev/event0 -- userspace input device.  Mouse +
// keyboard ISRs (via gui_init's listener) push Events; userspace reads them.
g_devfs.add_node("event0", &cinux::input::input_event_device_ops());
// 块设备走另一条:
for (uint32_t i = 0; i < cinux::drivers::BlockRegistry::count(); ++i) {
    g_devfs.add_block_node(cinux::drivers::BlockRegistry::name_at(i),
                           cinux::drivers::BlockRegistry::device_at(i));
}
```

`add_node` 是字符设备路径,`add_block_node` 是块设备路径(085 讲过),底层都走 [devfs.cpp](../../../kernel/fs/devfs/devfs.cpp#L319-L330) 的 `register_node`:

```cpp
void DevFs::add_node(const char* name, InodeOps* ops) {
    register_node(name, ops);
}

void DevFs::add_block_node(const char* name, cinux::drivers::IBlockDevice* dev) {
    // ...
    auto* ops = new BlockDevOps(dev);  // 块设备包一层 BlockDevOps
    block_dev_ops_[block_dev_count_++] = ops;
    register_node(name, ops);
}
```

字符设备的 ops 是写死的全局单例(`g_input_event_ops`、`g_fb_dev_ops`),块设备每次 `new` 一个 `BlockDevOps` 包住 `IBlockDevice`。两条路径、一张 `nodes_[]` 表。子目录(`/dev/pts/<N>`)走的是另一条 `set_dynamic_lookup` 路径——084 讲过 PTY 的 dynamic resolver,本章的 `/dev/event0` 是 flat 节点,不走那条。

### input 源分层(只画到接缝)

最后把输入侧的分层画清楚——014 详讲过 PS/2 scancode 不重讲,这里只点到 push_event 是接缝:

```text
硬件层    8042 PS/2 端口常量(ps2.hpp)        USB HID boot descriptor(hid_boot.hpp)
              ▼                                      ▼
解码层    mouse.cpp 三字节包 → MouseEvent        keyboard.cpp scancode → KeyEvent
              ▼                                      ▼
event 层   cinux::gui::Event(统一结构,event.hpp)
              ▼                                      ▼
              └────────── push_event ───────────────┘   (接缝:解码完 → 送出去)
                              ▼
                        /dev/event0
                              ▼
                        用户态 ring3
```

push_event 是「解码完→送出去」的接缝——驱动层不知道有用户态,用户态不关心驱动怎么解的,中间靠 `Event` 这个统一结构 + `push_event` 这一刀切开。CODING-TASTE §14:driver 不沾 GUI 依赖,接缝放在 listener / dual-write 那行,不是塞进驱动内部。

## 调试现场

这一节的坑都来自实机现场,每个三段:现象 / 根因 / 验法。

### 坑 1:read 返 -EFAULT,但 log 显示 pop 成功

**现象**。用户态 `read("/dev/event0", buf, sizeof(Event))` 返 `-EFAULT`,但内核 log 打出 `[input] read pop type=3 copy=0 ret=24`——`pop` 成功(type=3 是 MouseUp,ret=24 是 sizeof(Event)),但 `copy=0`。

**根因**。`read` 实现里误用了 `cinux::user::copy_to_user` 写 staging buffer:

```cpp
// 错的写法:
cinux::user::copy_to_user(buf, &ev, sizeof(ev));  // buf 是 kernel staging!
```

`buf` 是 `sys_read` 提供的 **kernel staging buffer**——`sys_read` 之后自己 `copy_to_user` 把 staging 搬到真用户指针。`copy_to_user` 的 `is_user_vaddr` 会拒绝 kernel 地址(它就是设计来挡 kernel pointer 闯用户态的),返 false,read 返 `Fault`,sys_read 翻成 `-EFAULT`。

**验法**。看 log 的 `pop=N copy=0` 对比:pop 成功但 copy=0,说明 pop 后写 buf 那步失败了。解法是把 `copy_to_user` 改成 `std::memcpy(buf, &ev, sizeof(ev))`——kernel→kernel 拷贝,不经过 `is_user_vaddr` 检查。对比 `/dev/fb0` 的 `ioctl`:`arg` 是 user pointer(syscall 层透传),那里 `copy_to_user` 是对的。**read 的 buf 不是 user pointer,是 kernel staging**——这个性质差异是「教程即验证」最容易踩的坑。

### 坑 2:HPET ENABLE 位写了读回恒 0

**现象**。`HPET::init` 里 `write32(kHpetRegGeneralConfig, read32(...) | kHpetEnableCnf)`,期待主计数器开始递增,但读回 Config 寄存器恒 0,counter 也不动。

**根因**。寄存器偏移错了——第一版按「密集布局」把 Config 放 `0x008`,但 HPET 通用寄存器**间距 `0x10`**,Config 在 `0x010`。`0x008` 落在 reserved 区,写全丢,读回 0。

**验法**。改偏移 `0x008` → `0x010`,读回非 0 且 counter 递增,定位完成。诊断权威是 QEMU 源码 `hw/timer/hpet.c` 的 `HPET_CFG` 宏——它定义就是 `0x010`。诊断时还有个不对称的线索可加速定位:对 `0x000` 的**读**是对的(能拿到 `0x9896808086A201`,高 32 位 `0x00989680` 正是 100MHz 周期 fs 值),只有对 `0x008` 的**写**丢——这个「读对写错」的不对称指向「读偏移对、写偏移错」,而不是「整个 HPET 没响应」。教训:**写不生效先核偏移**,硬件文档(或 QEMU 源码)的寄存器偏移是权威,别按「密集布局」猜。

### 坑 3:64-bit MMIO 写被丢

**现象**。想优化 HPET 计数器读写,直接用一次 64-bit 写测试——`write64(kHpetRegMainCounter, 0xDEADBEEF...)`,读回是 0;但同样的值用 32-bit 写 `0x0F0` 那个偏移能写进、能读回。

**根因**。QEMU(和部分真机)**丢 64-bit MMIO 写**,虽然 64-bit 读碰巧能用(QEMU 内部拆成两次 32-bit 读再拼)。所以 `mmio32_*` helper 锁死访问宽度,64-bit 主计数器是「两次 32-bit 半读 + rollover 保护」(`hpet.cpp:49-60`)。

**验法**。同样地址,拆两次 32-bit 写 halves,读回拼起来对——就定位是访问宽度问题。教训:**MMIO 一律 32-bit**,helper 名字带 32 就是这个教训的固化。

### 坑 4:用户态 mirror 的 Event 布局没对齐

**现象**。用户态 struct 自己声明:

```c
struct Event {
    uint8_t  type;
    MouseEvent mouse;  // 用户自己声明,字段顺序跟内核一样
};
```

`read` 返 24 字节也对,但解出来的 `type` 是乱码,鼠标坐标也错位。

**根因**。先说**不是 padding 的锅**——这是最常见的误判。Cinux 内核的 `cinux::gui::Event` 是 `EventType type_;` 后跟一个 union,见 [event.hpp](../../../kernel/gui/event.hpp#L89-L96):

```cpp
struct Event {
    EventType type_;           // uint8_t enum
    union {
        MouseEvent mouse;   // 首字段 int32_t x —— 4 字节对齐
        KeyEvent   key;
    };
};
```

`EventType` 是 `uint8_t`,后面 union 首字段是 `int32_t`——C++ ABI 自动给 `type_` 后面塞 3 字节 padding 把 union 对齐到 4 字节边界。**两边用同一套 ABI(同一个 gcc -std=c++17),padding 两边自动一致**——所以 `sizeof(Event)` 两边都是 24,布局天然对齐。真正的坑是**字段顺序、字段类型、字段数量没严格 mirror**:

- `MouseEvent` 的字段顺序写反(内核是 `x/y/dx/dy/buttons/left/right/middle`,用户态写成 `x/y/buttons/dx/...`)
- `KeyEvent` 内部某 `bool` 被用户态写成 `int`(类型不同 → sizeof 变 → 后面字段全错位)
- 漏了 `dx`/`dy` 或 `shift`/`ctrl`/`alt`(字段数对不上)

**验法**。首要判据是 `static_assert(sizeof(Event) == 24)`——如果用户态 sizeof 不是 24,字段数/类型肯定错了。再用 `static_assert(offsetof(Event, mouse) == 4)` 比对偏移精修。**千万别上 `__attribute__((packed))`**——packed 会强行取消那 3 字节 padding,让用户态 `sizeof(Event)` 变成 21,跟内核 24 不一致,这才真错位。这条不是 Cinux 的锅——所有跨 ring 的 ABI 都得 mirror 字段(Linux `input_event` 也一样),但 Cinux 单一编译器、单一 ABI 下,padding 不是问题,字段顺序/类型才是。

### 坑 5:fork 后子进程写 fb 没反应(进阶)

**现象**。父进程 `mmap("/dev/fb0")` 拿到虚拟地址画屏幕 OK;`fork` 后**子进程**写同一虚拟地址,屏幕不更新。

**根因**(进阶,串内存管理章)。`fork` 时默认会把父进程的可写页标 CoW(Copy-on-Write)——清 `FLAG_WRITABLE`、设 COW 标志,这样父或子任一方写时触发 `#PF`,fault handler 分配新 RAM 页、复制内容、改映射。可 IoPhys VMA 的「物理页」是**设备显存**,不是 RAM——CoW 截胡的话,父进程写的是新分配的 RAM 副本,根本没写到真 framebuffer,屏幕自然不更新。

**解法**。`fork` 对 `FLAG_PCD` leaf 跳 CoW(直接共享原物理映射,设备内存本来就该共享)。见 [fork.cpp](../../../kernel/proc/fork.cpp#L89-L94):

```cpp
// F-GUI-USERSPACE batch 1: device (IoPhys) pages -- identified by
// FLAG_PCD, which only uncached device mappings carry -- are shared
// verbatim: NO CoW (writing the framebuffer must hit the real fb,
// not a private RAM copy) and NO pte_count (device memory is not
// PMM-managed, so there is nothing to bump and nothing to free).
if (entry_flags & FLAG_PCD) {
    dst_table[i].raw = src_table[i].raw;
    continue;
}
```

这条在 v1.0.0 已经落地(execve 路径对 IoPhys 页也有同款跳过,见 [execve.cpp](../../../kernel/proc/execve.cpp#L122-L123))。`IoPhys` 标志的另一重含义——不仅 page-fault 看它跳 PMM,fork/execve 也看它跳 CoW——是「设备内存不归 PMM 管」这条规矩在三处入口(fault/fork/execve)的一致体现。

## 验证

三层验证,每层明说测什么不测什么。

**第一层:host 单测,纯逻辑。** input event 的纯逻辑是 `RingBuffer` 算术(push/pop 边界、满静默丢)、`FbScreenInfo` 字段填充(4 字段对)、`FramebufferDevOps::mmap` 的边界检查(越界返 `EINVAL`、`offset+length` 溢出保护)。这层靠的是 host 可链的纯逻辑,不碰 scheduler/wait_queue/copy_to_user——那些是 kernel-only。host 单测罩不到 read/poll 的阻塞语义(那要 scheduler)。

**第二层:kernel 内测,QEMU 里跑真码。** test kernel 注入事件(`smoke_entry` mock push `MouseMove(123,45)` + `KeyDown('A')`),fork+execve 一个用户态 demo `/input_event_test`,demo `open("/dev/event0")` + `read` 2 个事件,验 type 和 payload 对——这一层把 push_event → RingBuffer → read → memcpy staging → sys_read copy_to_user 全链路跑通。dev note 记录这条 smoke 在两腿(单核 + `-smp 2`)各 5/5 iters PASS。同时还有一条平行的 `/dev/fb0` smoke:test kernel fork+execve `/fb_mmap_test`,demo `open("/dev/fb0")` + `ioctl(FBIOGET_SCREENINFO)` 拿屏几何 + `mmap` 拿虚拟地址 + 写读一个像素——验证 ioctl + mmap + page-fault IoPhys 分支全链路。两条 smoke 在 test kernel 里都真跑了,见 [main_test.cpp](../../../kernel/test/main_test.cpp#L300-L345) 的 `/fb_mmap_test` 段和 [main_test.cpp](../../../kernel/test/main_test.cpp#L352-L414) 的 `/input_event_test` 段。

**第三层:`make run` 冒烟。** `ls /dev` 见 `event0`/`fb0`,用户态 GUI host(`/cinux_gui_host`)open + mmap + read 端到端跑起来——见 [desktop_launch.cpp](../../../kernel/gui/desktop_launch.cpp#L36-L58),`launch_userspace` fork+execve 这个 host 进程,它 open `/dev/fb0` + `/dev/event0` 构 widget tree。

**boot 与 test kernel 的真实差异**。前面几章(064 那批)常说「test kernel 不走 boot,验不了 boot 那个 init」——本章这条不适用。test kernel 在 [main_test.cpp](../../../kernel/test/main_test.cpp#L237) 显式调了 `cinux::fs::devfs::init()`(注释明说「re-mount /dev (vfs_mount_init cleared it) so /dev/fb0 resolves」),并且在 [main_test.cpp](../../../kernel/test/main_test.cpp#L179) `musl_hello_smoke_entry` 里初始化了一个 test framebuffer——所以 `/dev/event0`、`/dev/fb0` 在 test kernel 里都解析得到,smoke 才跑得起来。真正的 boot/test 差异在别处:boot 走真 bootloader 的 VBE mode 设置,framebuffer 是真显存;test kernel 的 framebuffer 是为 smoke 搭的测试实例(几何参数可能跟 boot 不同)。production 真显存 + 真鼠标键盘,还是得 `make run` 起真内核。

**诚实说哪些 headless 测不到**:

- **poll 路径只设计未实跑**——dev note follow-up 明记「poll path 未跑机制测(smoke 只 read);批3 用户态 host 用 poll 时再验」。本章的 poll 设计(pipe 范式 + 同锁原子)讲透了,但「真在 QEMU 里跑过 poll 阻塞唤醒」没盖到。
- **mock push vs 真鼠标**——smoke 是 test kernel 的 `smoke_entry` mock push,不是真鼠标/键盘硬件中断。production 跑真鼠标/键盘时端到端验,要 `make run` 起真内核。

## 这章没做的

- **`/dev/input/event0` 子目录**:DevFS 现在不为 input 子树投影层级(PTY 走 `dynamic_lookup` 投 `/dev/pts/<N>`,但 input 没有对应的 resolver)。`/dev/event0` 是 flat 节点,不是 Linux 那种 `/dev/input/event0`。要支持子路径,得给 input 写专属 dynamic resolver,是明确 follow-up。
- **多设备/多 seat**:整个系统就一个 `/dev/event0` 单例(`InputEventDevice::instance()`)。多设备(两套鼠标键盘)、多 seat(多用户控制台),不在 v1.0.0 范畴。
- **`bpp` 暴露给用户改**:VBE mode `0x144` 32-bpp XRGB 写死,不支持用户态切分辨率/像素格式。
- **`poll` 实跑验证**:设计到位,QEMU 实跑待 ring3 GUI host 用 poll 时验。
- **`fb_var_screeninfo` 完整字段**:只回 4 个字段(width/height/pitch/bpp),`xoffset`/`red.length`/`pixclock` 那些 Linux 字段不建模。
- **NVMe 设备接口**:077 讲过 NVMe 驱动,不放本章。

## 小结

- **两类设备接口形态**:`/dev/event0` 是「事件流」(read/poll),`/dev/fb0` 是「内存窗口」(mmap/ioctl)。同一个 `InodeOps` 基类派生,差异化全在 override 哪几个虚函数——没 override 的走基类默认 `NotImplemented`,基类替你拒掉。
- **`/dev/event0` 的 MPSC**:双生产者(mouse IRQ12 + keyboard listener)打破 GUI SPSC 队列的单生产者假设,被迫上 `RingBuffer` + `Spinlock`;同一把锁还兼任 `prepare_to_wait` 的 waiter-lock,一锁双用闭合 lost-wakeup。
- **`/dev/fb0` 的 mmap 只回物理地址**:真正建 PTE 是 page-fault 的活,带 `FLAG_PCD` uncached——显存是设备内存不是 RAM,cache 它会让写像素只动 cache line。`IoPhys` 标志是「这不是 RAM 页别动」的唯一标识,PMM/munmap/fork/execve 全绕开(fork 跳 CoW 已在 v1.0.0 落地)。
- **read 的 buf 是 kernel staging 不是 user pointer**:必须 `std::memcpy`,绝不能 `copy_to_user`(`is_user_vaddr` 拒 kernel 地址)。对比 ioctl 的 `arg` 是真 user pointer,直接 `copy_to_user`——两种 buf 性质两样。
- **MMIO 范式 `mmio32_read/write`**:`volatile uint32` deref + byte-offset,`volatile` 管编译器优化,uncached 管 CPU cache;byte-offset 跟硬件文档一字对齐(HPET Config 在 `0x010` 不在 `0x008`),32-bit 锁死访问宽度(QEMU 丢 64-bit 写)。
- **诚实边界**:`/dev/event0` flat 不子目录、`Event` 自定义结构要 mirror(字段顺序/类型,padding 自动一致别上 packed)、`poll` level-triggered、队列满静默丢、`poll` 路径只设计未实跑、NVMe 不放(077)。

---

### 参考

- OSDev wiki:[Memory Map MMIO](https://wiki.osdev.org/Memory_Map_MMIO)、[Framebuffer](https://wiki.osdev.org/Framebuffer)、[evdev](https://wiki.osdev.org/evdev)
- Intel SDM Vol.3 第 10 章 LAPIC、第 14 章 HPET(xAPIC MMIO 寄存器布局、HPET General Config 偏移权威)
- QEMU 源码:`hw/timer/hpet.c`(`HPET_CFG = 0x010` 宏,诊断权威)
- 本 tag 源码:
  - [mmio.hpp](../../../kernel/drivers/mmio.hpp)(MMIO 32-bit 助手全文)
  - [input_event_device.hpp](../../../kernel/drivers/input/input_event_device.hpp) / [input_event_device.cpp](../../../kernel/drivers/input/input_event_device.cpp)
  - [fb_dev.hpp](../../../kernel/drivers/video/fb_dev.hpp) / [fb_dev.cpp](../../../kernel/drivers/video/fb_dev.cpp)
  - [framebuffer.hpp](../../../kernel/drivers/video/framebuffer.hpp) / [framebuffer.cpp](../../../kernel/drivers/video/framebuffer.cpp)
  - [page_fault.cpp](../../../kernel/arch/x86_64/page_fault.cpp) IoPhys 分支
  - [fork.cpp](../../../kernel/proc/fork.cpp) / [execve.cpp](../../../kernel/proc/execve.cpp) IoPhys 跳 CoW
  - [local_apic.cpp](../../../kernel/drivers/apic/local_apic.cpp) / [hpet.cpp](../../../kernel/drivers/hpet/hpet.cpp) MMIO 消费侧
  - [devfs_init.cpp](../../../kernel/fs/devfs/devfs_init.cpp) 注册行
  - [mouse.cpp](../../../kernel/drivers/mouse/mouse.cpp) / [gui_init.cpp](../../../kernel/gui/gui_init.cpp) dual-write
  - [sys_mmap.cpp](../../../kernel/syscall/sys_mmap.cpp) device probe
  - [main_test.cpp](../../../kernel/test/main_test.cpp) smoke 段