---
title: 01 · 导引 + 设计图
---

# 导引 + 设计图

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

