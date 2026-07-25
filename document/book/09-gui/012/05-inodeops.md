---
title: 05 · InodeOps 差异化 + 调试 + 收尾
---

# InodeOps 差异化 + 调试 + 收尾

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
