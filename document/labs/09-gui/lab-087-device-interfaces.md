---
title: Lab 087 · 用户态拿到事件、画到屏幕
---

# Lab 087 · 用户态拿到事件、画到屏幕

> 对应 `document/book/08-filesystem/087-userspace-device-interfaces.md`。验证档 **A 档**:这一章交付的是「用户态能 `read` `/dev/event0` 拿事件、能 `mmap` `/dev/fb0` 画像素」。核心机制靠源码阅读 + host 纯逻辑测罩(队列算术、字段填充、边界检查),端到端链路靠 kernel 内测 smoke(`/fb_mmap_test` + `/input_event_test`,test kernel 重挂了 `/dev` 并初始化了测试 framebuffer)+ `make run` 起真内核冒烟(真 VBE + 真鼠标键盘)。

## 目标

确认六件事:

1. **`/dev/event0` 是 InodeOps 子类**:`InputEventDeviceOps` 只 override `read`/`poll_events`/`poll_detach_waiter`,基类接口一行不动;
2. **双生产者逼出 Spinlock**:`InputEventDevice` 自带 `RingBuffer<Event,128>` + `Spinlock`(不复用 GUI 的 SPSC EventQueue);同一把锁兼任 `prepare_to_wait` 的 waiter-lock;
3. **`/dev/fb0` 的 mmap 只回物理地址**:`FramebufferDevOps::mmap` 校验越界后 `return fb->phys_base() + offset`,**不建 PTE**;
4. **IoPhys VMA + page-fault 落 uncached PTE**:device mmap 给 VMA 打 `IoPhys` + 存 `phys_base`,用户态首次写触发 `#PF`,`page_fault.cpp` 的 IoPhys 分支 `map_nolock + FLAG_PCD`(不调 PMM);fork/execve 对 `FLAG_PCD` 跳 CoW;
5. **`ioctl(FBIOGET_SCREENINFO)` 回 4 字段**:width/height/pitch/bpp=32,`copy_to_user` 直写 user arg(不是 staging);
6. **boot 真挂两个节点 + test kernel 也挂**:`make run` 见 `[DEVFS] mounted`,`ls /dev` 见 `event0`/`fb0`;test kernel 在 `main_test.cpp:237` 也调了 `devfs::init()`(专为让 `/dev/fb0` 解析),所以 smoke 能跑。

## 步骤

### 1. 源码确认:InodeOps 子类的 override 子集

```bash
sed -n '81,88p' kernel/drivers/input/input_event_device.hpp
sed -n '23,29p' kernel/drivers/video/fb_dev.hpp
```

应看到 `InputEventDeviceOps : public InodeOps` 只 override `read` + `poll_events` + `poll_detach_waiter`;`FramebufferDevOps : public InodeOps` 只 override `mmap` + `ioctl`。两个子类都**没** override `write`/`create`/`mkdir`——那些走基类默认。

确认基类默认:

```bash
sed -n '46,59p' kernel/fs/inode.cpp
```

应看到 `InodeOps::ioctl` 和 `InodeOps::mmap` 默认都返 `NotImplemented`,注释明说 sys\_\* 会翻成 `-ENOTTY`/走 file-backed 路径。「`/dev/fb0` 没有 read 语义」就是基类替你拒掉,不是写了个空 read。

### 2. 双生产者逼出 Spinlock

```bash
sed -n '1,20p' kernel/drivers/input/input_event_device.hpp
sed -n '44,72p' kernel/drivers/input/input_event_device.hpp
```

头注释应自陈 MPSC 设计决策:「two producers (mouse IRQ12 + keyboard via the GUI listener), so the lock-free SPSC EventQueue used by the GUI cannot be shared here」。class 体里应看到 `RingBuffer<Event, kInputEventQueueSize> events_` + `Spinlock lock_` + `Task* read_waiters_`。

确认 mouse.cpp 的 7 处 dual-write:

```bash
grep -n "push_event" kernel/drivers/mouse/mouse.cpp
```

应看到 7 个命中(`update_absolute` 内 move + 3 down + 3 up),分布在 `281/292/301/310/321/330/339`,每处都是 `g_event_queue_.enqueue(ev)` 紧跟 `InputEventDevice::instance().push_event(ev)`。

确认 keyboard 走 listener(零侵入 keyboard.cpp):

```bash
grep -n "push_event\|on_key_event\|register_key_listener" kernel/gui/gui_init.cpp
```

应看到 `on_key_event` 里 push + `Keyboard::register_key_listener(on_key_event)`。`keyboard.cpp` 本身应**零** `push_event` 命中——driver 不沾 GUI 依赖(§14)。

### 3. read 路径:memcpy staging + prepare_to_wait

```bash
sed -n '52,90p' kernel/drivers/input/input_event_device.cpp
```

三个要点逐个核:

- `count < sizeof(Event)` 返 `InvalidArgument`(evdev 语义);
- `prepare_to_wait(self)` 在 `irq_guard` 内(自标 `Blocked` 原子,lost-wakeup 闭合),`schedule_blocked()` 在锁外(真正切走);
- `std::memcpy(buf, &ev, sizeof(ev))` 写 staging——**不是** `copy_to_user`(那是坑 1 的现场)。

对照 ioctl 的 arg 是 user pointer:

```bash
sed -n '49,67p' kernel/drivers/video/fb_dev.cpp
```

应看到 `copy_to_user(reinterpret_cast<void*>(arg), &info, sizeof(info))`——`arg` 是 syscall 透传的 user pointer,直接 copy_to_user 是对的。**read 的 buf 是 kernel staging,ioctl 的 arg 是 user pointer——性质两样**。

### 4. mmap 钩子:只回物理地址

```bash
sed -n '35,47p' kernel/drivers/video/fb_dev.cpp
sed -n '17,23p' kernel/drivers/video/framebuffer.cpp
sed -n '95,101p' kernel/drivers/video/framebuffer.hpp
```

fb_dev.cpp 的 mmap 应只做三件事:拿 `system_framebuffer()`、越界检查(先 `offset > size` 再 `size - offset`,顺序防溢出)、`return fb->phys_base() + offset`。**全函数找不到建 PTE 的代码**——这是 demand paging,mmap 是登记意图。

`phys_base_` 是 `Framebuffer::init` 时存的 VBE PhysBasePtr(013 讲过大页映射,本章只消费这个字段)。

### 5. sys_mmap + page-fault:IoPhys 分支

```bash
sed -n '131,143p' kernel/syscall/sys_mmap.cpp
sed -n '181,183p' kernel/syscall/sys_mmap.cpp
sed -n '191,196p' kernel/syscall/sys_mmap.cpp
sed -n '63,68p' kernel/mm/vma.hpp
sed -n '256,283p' kernel/arch/x86_64/page_fault.cpp
```

sys_mmap 应看到 device probe(inode->ops->mmap 调钩子,`NotImplemented` 落 file-backed 路径,在 `131-143`)+ VMA 打 `IoPhys` 标志(`181-183`)+ 存 `vma->phys_base`(`191-196`,不持 inode ref)。`IoPhys = 1 << 7` 定义在 vma.hpp。

page_fault.cpp 的 IoPhys 分支应:`io_phys = phys_base + (virt_page - vma->start)` 算物理地址,flags 带 `FLAG_PCD`(uncached)+ `FLAG_USER` + `FLAG_WRITABLE` + `FLAG_NX`,`map_nolock` 落 PTE。**不调 PMM、不进 page cache、不 `pte_count_inc`**——设备内存不归 PMM 管。

### 6. fork/execve 跳 CoW(v1.0.0 已落地)

```bash
sed -n '89,94p' kernel/proc/fork.cpp
sed -n '122,123p' kernel/proc/execve.cpp
```

fork 应看到 `if (entry_flags & FLAG_PCD) { dst_table[i].raw = src_table[i].raw; continue; }`——IoPhys 页直接共享原物理映射,跳 CoW、跳 pte_count。execve 同款跳过。注释明说「writing the framebuffer must hit the real fb, not a private RAM copy」。

### 7. ioctl FBIOGET_SCREENINFO

```bash
sed -n '23,37p' kernel/drivers/video/fb_dev.hpp
sed -n '27,32p' kernel/drivers/video/fb_dev.cpp
sed -n '49,67p' kernel/drivers/video/fb_dev.cpp
```

`kFbioGetScreenInfo = 0x4600` 是 Cinux 自定义常量(数值碰巧和 Linux `FBIOGET_VSCREENINFO` 一致,struct 布局不同)。`FbScreenInfo` 只有 4 字段(width/height/pitch/bpp),`bpp` 写死 32(VBE mode `0x144`)。非该 request 返 `NotImplemented` → `-ENOTTY`。

### 8. DevFS 注册(test kernel 也挂)

```bash
sed -n '161,172p' kernel/fs/devfs/devfs_init.cpp
sed -n '319,330p' kernel/fs/devfs/devfs.cpp
sed -n '237,238p' kernel/test/main_test.cpp
sed -n '179,186p' kernel/test/main_test.cpp
```

devfs_init.cpp 应看到 `add_node("ptmx")` + `add_node("fb0")`(`164`)+ `add_node("event0")`(`167`)+ block loop,跟 064 的 null/zero/console 同一个 `add_node` 机制;块设备走 `add_block_node`(085 讲过);子目录 `/dev/pts/<N>` 走 `set_dynamic_lookup`(084 讲过,本章 fb0/event0 是 flat 节点不走那条)。

main_test.cpp 应看到 test kernel **也**调 `cinux::fs::devfs::init()`(注释明说「re-mount /dev (vfs_mount_init cleared it) so /dev/fb0 resolves」),并在 `musl_hello_smoke_entry` 里 init 了测试 framebuffer——所以 `/dev/event0`、`/dev/fb0` 在 test kernel 里都解析得到,smoke 才跑得起来。**别照搬 064 那条「test kernel 不挂 /dev」的判据**——本章不适用。

### 9. boot 真挂

```bash
sed -n '182,186p' kernel/main.cpp
```

应看到 `Framebuffer fb; fb.init(*boot_info); set_system_framebuffer(&fb);`——`/dev/fb0` mmap 靠这个 system singleton 解析物理地址。

```bash
make run
```

起 QEMU,boot 序列应打 `[BIG] Framebuffer initialised: <W>x<H> <bpp>bpp` + `[DEVFS] mounted at /dev (<N> nodes)`。`ls /dev` 应见 `event0`/`fb0`。

### 10. kernel 内测 smoke(test kernel 真跑)

```bash
sed -n '300,345p' kernel/test/main_test.cpp
sed -n '352,414p' kernel/test/main_test.cpp
```

应看到两条 smoke:

- `/fb_mmap_test`(`300-345`):fork+execve 用户态 demo,demo `open("/dev/fb0")` + `ioctl(FBIOGET_SCREENINFO)` + `mmap` + 写读一个像素,验 ioctl + mmap + page-fault IoPhys 全链路。
- `/input_event_test`(`352-414`):每轮 mock push `MouseMove(123,45)` + `KeyDown('A')`,fork+execve demo,demo `open("/dev/event0")` + `read` 2 事件,验 type + payload。

dev note 记录两条 smoke 在两腿(单核 + `-smp 2`)各 5/5 iters PASS。

### 11. 进阶:写个最小用户态 GUI demo(A 档动手)

骨架,不给答案:

```c
// 用户态镜像 Event 布局(字段顺序+类型严格 mirror,padding 自动一致别上 packed)
typedef struct {
    int32_t x, y, dx, dy;
    uint8_t buttons;
    bool left, right, middle;
} MouseEvent;
typedef struct {
    char ascii; uint8_t scancode;
    bool pressed, shift, ctrl, alt;
} KeyEvent;
typedef struct {
    uint8_t type_;   // EventType
    union { MouseEvent mouse; KeyEvent key; };
} Event;   // static_assert(sizeof(Event) == 24)

typedef struct { uint32_t width, height, pitch, bpp; } FbScreenInfo;
#define FBIOGET_SCREENINFO 0x4600   // 必须用 Cinux 这个值,struct 跟 Linux vscreeninfo 不同

int main() {
    // 1. ioctl 拿屏几何
    int ffd = open("/dev/fb0", O_RDWR);
    FbScreenInfo info;
    ioctl(ffd, FBIOGET_SCREENINFO, &info);
    // 2. mmap 拿到帧缓冲虚拟地址
    uint8_t* fb = mmap(NULL, info.pitch * info.height, PROT_READ|PROT_WRITE,
                       MAP_SHARED, ffd, 0);
    // 3. 画一个红矩形到 fb(0xFFFF0000 = XRGB 红)
    // 4. open /dev/event0 + read 拿鼠标 move 事件
    int efd = open("/dev/event0", O_RDONLY);
    Event ev;
    while (read(efd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type_ == /* MouseMove */) {
            // 擦旧矩形、按 ev.mouse.x/y 画新矩形
        }
    }
}
```

**验证判据**:屏幕真出红矩形 + 矩形跟着鼠标动 = mmap 绑 VMA + read 事件链路全通。

**踩坑方向(不给答案,给方向)**:

- **Event 字段顺序/类型**:用户态 struct 必须 mirror `cinux::gui::Event` 的字段顺序和类型(`MouseEvent`: `x/y/dx/dy/buttons/left/right/middle`;`KeyEvent`: `ascii/scancode/pressed/shift/ctrl/alt`)。用 `static_assert(sizeof(Event) == 24)` + `static_assert(offsetof(Event, mouse) == 4)` 比对。**别上 `__attribute__((packed))`**——会取消 padding 让用户态 sizeof 变 21,跟内核 24 不一致。
- **mmap 首次写才建 PTE**:`mmap` 返回的虚拟地址,第一次写像素才触发 `#PF` 落 PTE(demand paging)。别在 mmap 钩子里找建表逻辑。
- **fork 后子进程写 fb 没反应**(进阶):IoPhys VMA 的 PTE 在 fork 时**已经**跳 CoW(fork.cpp:89-94),所以子进程应该能写屏。如果你跑出子进程写屏没反应,先核 fork 路径的 `FLAG_PCD` 跳 CoW 是否真生效(本 tag 源码已落地),再核 mmap 的 VMA 是否真打了 `IoPhys` 标志(sys_mmap.cpp:181-183)。
- **`kFbioGetScreenInfo` 必须是 `0x4600`**:用户态 mirror 这个常量,别照搬 Linux `FBIOGET_VSCREENINFO` 的宏(数值碰巧一致,但 struct 布局不同)。

## 验收清单

- [ ] `input_event_device.hpp:81-88` `InputEventDevOps` 只 override read/poll;`fb_dev.hpp:23-29` `FramebufferDevOps` 只 override mmap/ioctl;`inode.cpp:46-59` 默认 `NotImplemented`。
- [ ] `input_event_device.hpp:44-72` `RingBuffer<Event,128>` + `Spinlock` + `read_waiters_`;头注释自陈 MPSC 决策。
- [ ] `mouse.cpp` 7 处 dual-write(281/292/301/310/321/330/339);`gui_init.cpp` listener 双写;`keyboard.cpp` 零 `push_event`。
- [ ] `input_event_device.cpp:52-90` read 三要点:`count < sizeof` EINVAL + `prepare_to_wait` 锁内 + `memcpy` staging(非 copy_to_user)。
- [ ] `fb_dev.cpp:35-47` mmap 只回 `phys_base()+offset`,不建 PTE;越界检查顺序对(先 offset 后 size-offset)。
- [ ] `sys_mmap.cpp:181-183` VMA 打 `IoPhys`;`sys_mmap.cpp:191-196` 存 `vma->phys_base`;`page_fault.cpp:256-283` IoPhys 分支 `map_nolock + FLAG_PCD`,不调 PMM。
- [ ] `fork.cpp:89-94` / `execve.cpp:122-123` 对 `FLAG_PCD` 跳 CoW(v1.0.0 已落地)。
- [ ] `fb_dev.cpp:49-67` ioctl FBIOGET_SCREENINFO 回 4 字段,`copy_to_user` 直写 arg;`kFbioGetScreenInfo=0x4600`。
- [ ] `devfs_init.cpp:164,167` add_node 注册 fb0/event0;`main_test.cpp:237` test kernel 也调 `devfs::init()`;`make run` 见 `[DEVFS] mounted`,`ls /dev` 见两节点。
- [ ] `main_test.cpp:300-345` `/fb_mmap_test` smoke + `main_test.cpp:352-414` `/input_event_test` smoke。
- [ ] A 档 demo:屏幕出红矩形 + 跟着鼠标动。

## 别做这些

- **别**照搬 064 那条「test kernel 不挂 /dev」判据——本章不适用。test kernel 在 `main_test.cpp:237` 显式调了 `devfs::init()`(注释明说为让 `/dev/fb0` 解析),并在 `musl_hello_smoke_entry` 里 init 了测试 framebuffer。boot 与 test kernel 的真实差异在「真 VBE vs 测试 framebuffer」,不在「挂不挂 /dev」。要验真 VBE + 真鼠标键盘,还是 `make run`。
- **别**用 `copy_to_user` 写 read 的 `buf`——它是 kernel staging(`sys_read` 提供),`is_user_vaddr` 会拒 kernel 地址,返 `-EFAULT`。用 `std::memcpy`。对比 `/dev/fb0` 的 `ioctl` arg 是真 user pointer,直接 `copy_to_user` 是对的。
- **别**以为 `poll` 路径在 QEMU 实跑过——dev note 明说「poll path 未跑机制测(smoke 只 read);用户态 host 用 poll 时再验」。设计到位,实跑待 ring3 host 用 poll 时盖。
- **别**照搬 Linux `fb_var_screeninfo` 的 struct 布局——Cinux 的 `FbScreenInfo` 只有 4 字段(width/height/pitch/bpp),数值常量 `0x4600` 碰巧一致但布局完全不同。用户态必须 mirror Cinux 的 4 字段。
- **别**以为 `Event` 跟 Linux `input_event` 一样——它是 Cinux 自己的 `cinux::gui::Event`(`EventType` 1 字节 + union),用户态要 mirror 字段顺序和类型。**别上 `__attribute__((packed))`**——padding 两边 ABI 自动一致,packed 反而会让 sizeof 变 21 跟内核 24 不一致。
- **别**在 mmap 钩子里找建 PTE 逻辑——那在 `page_fault.cpp` 的 IoPhys 分支。mmap 钩子只回物理地址,真正建表是用户态首次写触发 `#PF` 时做的。
- **别**给 IoPhys VMA 走 CoW——设备内存不归 PMM 管,fork 时 `FLAG_PCD` leaf 已跳 CoW(fork.cpp:89-94,v1.0.0 已落地)。否则父进程写屏被截胡到 RAM 副本,屏幕不更新。
- **别**用 64-bit MMIO 写——QEMU 丢 64-bit 写,helper 名字带 `32` 就是锁死访问宽度。64-bit 计数器拆两次 32-bit 半读 + rollover 保护。
- **别**把 HPET Config 寄存器当 `0x008`——它在 `0x010`(通用寄存器间距 `0x10`)。`0x008` 是 reserved 区,写全丢。QEMU `hw/timer/hpet.c` 的 `HPET_CFG` 宏是权威。诊断时记一条不对称:对 `0x000` 的**读**是对的(能拿 100MHz period),只有对 `0x008` 的**写**丢——「读对写错」指向偏移错。