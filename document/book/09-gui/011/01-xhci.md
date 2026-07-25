---
title: 01 · xHCI USB:一张环、一个 cycle bit,把键鼠接到桌面
---

# xHCI USB:一张环、一个 cycle bit,把键鼠接到桌面

> 054 把桌面 GUI 从内核里拔了出来,可那套桌面的输入还只有 PS/2 鼠标——键盘敲不响、USB 鼠标也不认。这一章给内核接上一个真正的 USB 主控制器驱动:xHCI。它是一整条链:先在 PCI 总线上把 xHCI 控制器认出来、复位、用 MSI-X 给它挂一个中断向量;再学会和控制器通信的「语言」——一种叫 TRB 的 16 字节描述符排成的环,靠一个 cycle bit 无锁握手;然后用这套语言把一个 USB 设备枚举出来(分 slot、填 context、下 Address Device 命令);最后解析 HID 鼠标/键盘/tablet 的输入报告,送进 054 那个 GUI 窗口管理器已经在消费的同一个事件队列。验证靠内核测试 `test_xhci`(找控制器、复位、给设备寻址、跑通 HID 鼠标)。
>
> 诚实的边界先说清:这套驱动在 QEMU 上跑通,真中断的送达在 QEMU+nested-KVM 下并不可靠(中断器使能位锁存不上),所以生产路径其实是 worker 线程在轮询事件环,MSI-X 的武装留着给真硬件;SuperSpeed(5 Gb/s)、hub 拓扑这些没做;BIOS 那套 USB Legacy Support 的所有权交接在 QEMU 上不需要(启动即 OS-owned),真机才补。

## 这章咱们要点亮什么

1. **xHCI 是什么、怎么从 PCI 认出来**:USB 3.0 时代的统一主控制器接口,靠 PCI 三段式 class/subclass/prog_if 精确识别,四块寄存器散在 BAR0 的不同偏移上。
2. **MSI-X 中断**:为什么不用老的共享中断线,而让设备往一段约定地址写一笔数据就算中断;怎么在 PCI 能力链表里找到它、给一个向量编程。
3. **TRB 和环**:host 和控制器之间所有的工作都描述成 16 字节的 TRB,排成环;两边不靠锁,只靠一个 cycle bit 握手。
4. **枚举一个设备**:slot_id、input/output context、为什么 xHCI 里 SET_ADDRESS 被一条控制器命令替掉了。
5. **HID boot 协议**:为什么不必解析每个设备冗长的 report descriptor,只认鼠标 4 字节 / 键盘 8 字节 / tablet 5 字节;以及 USB 输入怎么收进 054 的 GUI 事件队列。

## 先认识 xHCI:USB 3.0 时代的统一主控制器

USB 经历过四代主控制器接口——UHCI/OHCI(USB 1.x)、EHCI(USB 2.0)、xHCI(USB 3.0)。前几代各有各的寄存器布局和传输模型,到 xHCI 统一成一套。对内核来说,xHCI 控制器就是一块挂在 PCI 总线上的标准设备,得先把它从总线上的其它设备里挑出来。

怎么挑?USB 主控制器在 PCI 配置空间里都是 `class=0x0C`(serial bus)、`subclass=0x03`(USB),但这俩还不够——四代控制器共用这两个值。真正区分它们的是第三个字段 `prog_if`(programming interface):UHCI=`0x00`、OHCI=`0x10`、EHCI=`0x20`、xHCI=`0x30`。所以识别谓词必须三段全查:

```cpp
constexpr bool is_xhci_device(uint8_t cls, uint8_t sub, uint8_t prog_if) {
    return cls == PciClass::SERIAL_BUS && sub == PciClass::USB_SUBCLASS &&
           prog_if == PciClass::XHCI_PROG_IF;   // 0x0C / 0x03 / 0x30
}
```

（`pci.hpp:58`,常量在 `pci_config.hpp:57-59`。）只比 class+subclass 会把老的 UHCI/OHCI/EHCI 一起误命中——这是 xHCI 发现的头号坑,所以这个谓词被特意抽成纯 `constexpr` 函数,host 上能写单测把四种错误 prog_if 全试一遍。

找到之后第一件事不是碰它的寄存器,而是先在 PCI 配置空间的 COMMAND 寄存器里置两个位:`BUS_MASTER`(允许它做 DMA)、`MEM_SPACE`(允许它响应 MMIO)。这俩不置,后面所有寄存器访问都读到全 0,代码全对但控制器像死的——`xhci_controller.cpp:75` 的 init 第一步就干这个。

xHCI 的寄存器不像老控制器挤在一小块里,而是按访问特性拆成**四块**,都落在 BAR0 上,但起点不固定,要从只读的 Capability 块里现读出来:

- **Capability**(BAR0+0):只读,给出 `CAPLENGTH`(Operational 块偏移)、`HCSPARAMS1`(MaxSlots/MaxIntrs/MaxPorts)、`DBOFF`(doorbell 数组偏移)、`RTSOFF`(runtime 偏移)。
- **Operational**(BAR0+CAPLENGTH):可写,放 `USBCMD`/`USBSTS`/`CRCR`/`DCBAAP`/`CONFIG`,以及端口状态寄存器 PORTSC。
- **Runtime/Interrupter**(BAR0+RTSOFF):事件环靠它驱动(IMAN/IMOD/ERSTSZ/ERSTBA/ERDP)。
- **Doorbell**(BAR0+DBOFF):一个 `uint32` 数组,每 slot 一个,写它就是通知控制器干活(索引 0 = 命令环)。

初始化的核心是**先停再复位**。控制器在跑的时候是不能复位的,所以得先 `USBCMD=0`(清掉运行位),轮询等 `USBSTS.HCH`(HC Halted)置位; halted 之后才能写 `USBCMD.HCRST` 触发复位,再轮询等两个就绪信号——`USBSTS.CNR`(Controller Not Ready)清零 + `HCRST` 自清(硬件复位完自己把位清掉):

```cpp
op_regs_->usbcmd = 0;                       // 先停
for (...) if (op_regs_->usbsts & Usbsts::kHcHalted) break;
op_regs_->usbcmd = Usbcmd::kHcReset;        // 再复位
for (...) if (!(sts & Usbsts::kControllerNotReady) && !(run & Usbcmd::kHcReset)) break;
```

（`xhci_controller.cpp:104` 和 `:114`。）复位完控制器应停在 halted + CNR clear 的干净状态,后面才能往里装环、把它跑起来。轮询都带了超时上限,防止硬件卡死把启动卡住。

> **真机才有的步骤:BIOS 交接。** 真机上 BIOS 常用一个叫 USB Legacy Support 的扩展能力做键盘模拟(SMI 拦截 USB、假装成 PS/2 键盘给 OS)。OS 接管前必须做所有权交接,否则和 BIOS 抢控制器。但 QEMU 的 qemu-xhci 启动就是 OS-owned、没有 BIOS 模拟层,所以 Cinux 直接跳过这一步——这是诚实的 QEMU-only 简化,代码注释里标了「真机需补 USB Legacy Support handoff」。

## MSI-X:让设备用一次内存写就打断 CPU

xHCI 控制器干完活要打断 CPU,得有个中断机制。老式 PCI 中断(INTx)是物理引脚:多块设备挂在同一条 IRQ 线上,8259/IOAPIC 把线信号翻译成 CPU 中断,要共享、要扫描确认是谁发的、容易丢。MSI-X 反过来:**设备自己往一段约定好的物理地址写一笔数据,这笔写本身就是中断**——没有共享线,CPU 收到的就是「谁、什么向量」。

这套机制藏在 PCI 配置空间的能力链表里。每块 PCI 设备能挂一串「能力」,靠单链表串起来:STATUS 寄存器的 bit4 置位表示有链表,偏移 0x34 存第一个能力的位置;每个能力头是「一字节 id + 一字节 next 指针」,顺着 next 走直到 0。MSI-X 的 id 是 `0x11`。`find_capability` 就是这条遍历(带个 48 步硬上限防 next 指针成环死循环)。

找到 MSI-X 能力后,同一个能力给出的后续字段解码出全部布局:**Table Size**(条目数,注意是字段值+1)、**MSI-X Table 藏在哪个 BAR 的哪个偏移**、**PBA(待处理位图)的位置**。MSI-X Table 是一个 16 字节条目的数组,每项四个字段:消息地址(低/高)、消息数据、屏蔽位。给一个向量编程(`program_vector`)分四步:先屏蔽这条目(防编程中途设备投递)、写消息地址和数据、回读一次作内存屏障、再解除屏蔽。

消息地址编码是 xAPIC 的约定——基址 `0xFEE00000`,目标 APIC ID 放进 `[19:12]`:

```cpp
uint32_t xapic_message_address(uint8_t dest_apic_id) {
    return 0xFEE00000u | (static_cast<uint32_t>(dest_apic_id) << 12);
}
```

（`msix.cpp:81`,目标 APIC ID 0 = BSP。）消息数据就是向量号本身。Cinux 只用了 MSI-X Table 的 entry 0,把它编程成投递到 IDT 向量 `0x40`、目标 BSP:

```cpp
msix_.program_vector(0, kXhciIrqVector, 0);   // entry 0 -> vector 0x40, BSP
```

（`xhci_controller.cpp:230`。）`0x40` 是手挑的空闲位置,避开 `0x20-0x2F`(PIC IRQ)、`0x80`(sigreturn)、`0xE0`(resched IPI)、`0xFF`(LAPIC spurious)。这个向量在启动期就注册进了 IDT,早于 AP 起来,避开 IDT 竞态。

> **为什么 MSI-X 单独成一个子系统。** 它放在 `kernel/drivers/pci/` 而不是塞进 `usb/`,因为 MSI-X 是 PCI 通用能力——AHCI 将来也能用,xHCI 只是第一个消费者。有意思的是,这套「遍历能力链表 + 解码」的逻辑被刻意写成了接收一个 `ConfigReader` 函数指针的纯函数:内核里传真的 `PCI::pci_read`,host 单测传一个 mock。于是同一份代码既能链进内核、又能链进单测,`test_msix` 测的是真 `msix.cpp` 不是副本。这是内核驱动可测性的标准范式。

## TRB 和环:host 与控制器不用锁怎么通信

这是 xHCI 最核心、也最有意思的设计。

老式控制器(host 软件逐字节发 SETUP 包)已经过去了。xHCI 把 host 和控制器之间的**所有工作**——命令(Enable Slot、Address Device)、传输(SETUP/Data/Status)、事件(命令完成、传输完成)——统一描述成一种 16 字节的小结构,叫 **TRB**(Transfer Request Block):

```cpp
struct Trb {
    volatile uint64_t parameter;  // +0:  数据缓冲物理地址 / 命令参数 / 事件参数
    volatile uint32_t status;     // +8:  传输长度 / 完成码
    volatile uint32_t control;    // +12: [0]=Cycle, [15:10]=Type, 其它标志
};
static_assert(sizeof(Trb) == 16, "TRB must be 16 bytes");
```

（`xhci_trb.hpp:21`。)成员全 `volatile`,因为它同时被 CPU 和 DMA 控制器读写,编译器不能把读写缓存进寄存器。TRB 排成**环形队列**,host 和控制器各拿一头。

两边怎么知道「这条归谁读」?**不靠锁,靠 control 字段最低位那个 cycle bit。** 规则极简:生产者填好一个 TRB 后,把它的 cycle bit 设成自己当前的「cycle state」;消费者只读 cycle bit 等于自己期望值的 TRB。生产者把位翻过去就是「这条归你了」,消费者读完不碰位、靠生产者下一圈覆盖时重写来回收槽位。判满判空都落在这个位上。

有三种环,职责不同:

- **命令环**(command ring):host 生产、控制器消费,放管理命令,全控制器一条,物理地址写进 CRCR 寄存器。
- **事件环**(event ring):控制器生产、host 消费,放控制器干完活回报的事件,通过 ERST 段表描述。
- **传输环**(transfer ring):每个 slot 的每个端点一条,host 生产、控制器消费,放实际数据搬运指令。

命令环和传输环 host 是生产者,用一个 `TrbRing` 类实现:环存 N+1 个 TRB,末尾固定位置是一个 **Link TRB** 把环首尾连起来。enqueue 填到末尾时,重写 Link TRB、把 enqueue 归零、**翻转 PCS(生产者 cycle state)**:

```cpp
void TrbRing::enqueue(uint64_t parameter, uint32_t status, uint32_t control) {
    Trb* t = &storage_[enqueue_];
    t->control = control | (pcs_ ? kCycleBit : 0);   // 填当前 PCS
    ++enqueue_;
    if (enqueue_ == slots_) { write_link(); enqueue_ = 0; pcs_ = !pcs_; }   // 绕回 + 翻 PCS
}
```

（`xhci_ring.cpp:37`。)为什么要翻 PCS:第二圈重新填同一个槽位时,cycle 必须和第一圈相反,否则控制器会以为这些「旧」TRB 还归它处理。事件环控制器是生产者,没有 Link TRB,按 ERST 段大小回绕、翻 CCS;host 的 dequeue 看到 `cycle != CCS` 就知道环空了(`xhci_ring.cpp:64`)。

host enqueue 完,控制器并不会自动知道——得**敲 doorbell**。doorbell 是 BAR0+DBOFF 处的 `uint32` 数组,写它就是「有新 TRB 了,去看你那头的环」的唯一信号:

```cpp
cmd_ring_.enqueue(parameter, status, control);
doorbells_[0] = Doorbell::kTargetCommandRing;   // 通知控制器:命令环有新 TRB
```

（`xhci_controller.cpp:269`,索引 0 = 命令环。)一次命令的完整生命周期就这么串起来:host 在命令环 enqueue 一个命令 TRB → 写 doorbell[0] → 控制器 DMA 读环、执行、把结果写进事件环(一个 Command Completion Event)→ host 在 `poll_events` 里 dequeue 事件环、按「参数回显」认出这是自己那条命令的完成。三种环就这么协作。

## 枚举一个设备:slot、context、Address Device

控制器跑起来之后,接上一个 USB 设备,怎么把它认出来?xHCI 把传统 UHCI/EHCI 里 host 软件要操心的「逐字节发包、手动维护设备地址」大部分逻辑**下沉进了控制器硬件**。host 的活变成两件:为每个设备维护一块叫 **context** 的 DMA 内存,用「填字段 + 下命令」描述设备;再通过命令环驱动控制器的状态机。

枚举一个设备走这么几步(`usb_init.cpp:50` 的 `enumerate_port`):

1. **发现连接**:读端口寄存器 PORTSC,bit0 CCS=1 表示有设备。
2. **端口复位 + 测速**:写 PORT_RESET,轮询等它自清,读 PORTSC 的速度位(FS/LS/HS)。
3. **Enable Slot**:下一条命令(TRB type `kEnableSlot`=9),控制器分配一个 `slot_id`,在完成事件里返回。
4. **建 context + Address Device**:给这个 slot 填一块 input context,下 Address Device 命令。
5. **读描述符 + 配置**:在 EP0 上做 control transfer 读设备/配置描述符,找到 HID 接口和它的 interrupt-IN 端点,SET_CONFIGURATION + Configure Endpoint 把端点跑起来。

这里有个 xHCI 最容易被讲错的点。传统 USB 规范里 host 要发一个 `SET_ADDRESS` 标准请求给设备分配地址。**xHCI 里这个总线请求被一条控制器命令替掉了**——Address Device(TRB type 11)。host 只要把填好的 input context 物理地址塞进命令 TRB、把 slot_id 编进 control 字段、下命令,**控制器自己**给设备分配地址、在总线上完成寻址握手、把结果写进 device context。源码里特意保留了 `kSetAddress=5` 这个常量并标注:

```cpp
constexpr uint8_t kSetAddress = 5;   // xHCI 不发:控制器用 Address Device 命令寻址
```

（`usb_request.hpp:53`。)这就是 xHCI 把 host 负担下沉到硬件的核心设计:host 只描述想要的终态(input context),控制器完成总线时序。

input context 是什么?host 想改设备状态时不能直接改 device context(那是控制器写、host 读的输出),而是填一块 input context 再下命令,控制器校验后合并进去。input context 的布局是「Input Control Context(32B)+ 一份 device context 拷贝」。Input Control Context 有两组 flag:**DW0 = Drop flags**(要丢弃哪些 context)、**DW1 = Add flags**(要新建哪些 context)。Address Device 只要建 slot + EP0,所以:

```cpp
in[0] = 0;                              // Drop:什么都不丢
in[1] = input_add_flag(0) | input_add_flag(1);   // Add:slot + EP0
```

（`xhci_slot.cpp:97`。)这一对的顺序(Drop 在 DW0、Add 在 DW1)是枚举阶段最致命的坑——写反了 QEMU 直接返回 TRB Error,因为它校验 `ictx DW0==0 && DW1==0x3`。

> **EP0 上的 control transfer 永远是三阶段。** 所有标准请求(GET_DESCRIPTOR、SET_CONFIGURATION)都在 EP0 这个控制端点上走,一次 control transfer 由最多三条 TRB 组成:**SETUP**(内联 8 字节 SETUP 包)→ **Data**(有数据时)→ **Status**(零长度握手)。方向规则:GET_DESCRIPTOR 是 IN(设备→host),三阶段 = SETUP + Data(IN) + Status(OUT);SET_CONFIGURATION 无数据,两阶段 = SETUP + Status(IN)。Status 的方向永远是数据阶段的反向——这是 USB 协议的握手约定。

## HID boot 协议:免解析的固定报告

端点配好之后,设备的 interrupt-IN 端点就能收报告了。但 USB HID 规范允许每个设备自带一段叫 report descriptor 的描述,声明自己报告里每个字段是什么、几位、范围多大——完整解析它要一个状态机,对教学内核是负担。

规范为此预留了**boot protocol**:只要设备声明 `interface class=0x03`(HID)/`subclass=0x01`(boot)/`protocol=0x02`(鼠标)或 `0x01`(键盘),并接受 SET_PROTOCOL(boot) 类请求,它就保证输出**固定字节布局**。BIOS、UEFI、教学内核都靠这一层免解析:

- **鼠标**:3-4 字节。byte0 = 按键位图(左/右/中)、byte1 = int8 dx、byte2 = int8 dy、可选 byte3 = wheel。
- **键盘**:8 字节。byte0 = 修饰键位图(Ctrl/Shift/Alt/GUI)、byte1 reserved、byte2-7 = 最多 6 个同时按下的键的 HID Usage ID。
- **tablet**(QEMU usb-tablet,绝对指针):5 字节。byte0 = 按钮、byte1-2 = 16 位小端 X、byte3-4 = 16 位小端 Y,范围 0..32767。

```cpp
constexpr HidMouseReport decode_boot_mouse(const uint8_t* r) {
    return HidMouseReport{r[0], static_cast<int8_t>(r[1]), static_cast<int8_t>(r[2]),
                          static_cast<int8_t>(r[3])};
}
```

（鼠标/平板解码在 `drivers/mouse/hid.hpp:43`,键盘解码在同名的 `drivers/keyboard/hid.hpp`——两份 `hid.hpp` 不同目录,别找错。）有个容易踩的细节:HID 鼠标的 Y+ 是「远离用户」=屏幕向下,所以 dy **不取反**直接累加;这和 PS/2 鼠标相反(PS/2 的 Y+ 是物理向上,要取反)。这两条**相对位移**路径(USB 的 `inject_usb_motion` 和 PS/2 的 `apply_motion`)靠调用处是否取反来区分约定——它们在测试里都跑;生产指针则走另一条绝对路径,下面说。

报告怎么从设备流到内核?interrupt-IN 端点天生适合输入:host 提交一个 async interrupt-IN transfer(只入队 TRB + 敲门铃,不阻塞),然后挂起。设备没数据时 NAK(host 零 CPU),有数据时 xHCI 把报告 DMA 进内核缓冲、往事件环挂一个 Transfer Event。内核在 `poll_events` 里按 slot_id 把事件分发给该设备注册的 `TransferListener`,后者解码报告、注入事件队列、再 arm 下一个 transfer。整个输入就这么自驱动地流动。

**收口的地方**:054 那个 GUI 窗口管理器消费的 `cinux::gui` 事件队列,USB 输入最终也汇进这同一个队列——无论 PS/2 还是 USB,所有输入都进同一个队列,`pump()` 那头不区分来源。为了避免两个驱动抢同一个(单生产者)队列,USB 枚举成功后置 `usb_primary` 标志,PS/2 的中断处理函数查到这个标志就直接 return、不喂队列;USB 成为唯一生产者。反过来,没 xHCI 控制器时(比如 `run-kernel-test` 默认无 qemu-xhci)`usb::init` 优雅跳过,PS/2 继续当主输入。

> **生产指针是 usb-tablet,不是相对鼠标。** Cinux 在生产里枚举的指针设备是 QEMU usb-tablet(`UsbTablet`,绝对坐标)——它同样被 `find_boot_mouse` 认出来(usb-tablet 也呈现 HID boot-mouse 接口 3/1/2),但用 `decode_tablet` 解 5 字节绝对报告。上面那个相对鼠标 `decode_boot_mouse` 主要是 `test_hid_mouse` 用来验证 boot 鼠标解码本身的。为什么生产选 tablet:相对鼠标在虚拟机里有个老毛病——宿主光标和 guest 自画光标是两个独立光标,撞窗口边后 delta 丢失、绝对位置发散,点不准。usb-tablet 报绝对坐标(0..32767 线性映射到屏幕像素)根治它,guest 光标直接设到宿主光标的位置,两个光标合一。

## 诚实的边界:哪些没做完、哪些是 QEMU 简化

像前一章那样,把没做完的事说清楚。

**真中断在 QEMU 下其实没真送达。** xHCI 的中断器使能位(IMAN.IE)在 QEMU + nested-KVM 下锁存不可靠,MSI-X 中断在开发/测试环境里并不真触发。所以生产路径其实是 `gui_worker` 线程每帧轮询事件环(`poll_events`),MSI-X 的整套武装保留着、给真硬件或未来的 QEMU 用——代码注释明说了这一点。也就是说「向量真触发 → handler 真跑 → 中断计数上涨」这条链在本章的开发环境里没端到端验证,handler 在生产里其实是休眠的。`test_xhci` 验证的是 find/reset/address-device/hid-mouse 这套机制本身(靠轮询),不依赖真中断。

**BIOS 交接跳过了。** 前面说过,QEMU 启动即 OS-owned,所以没做 USB Legacy Support 的所有权协商。移植到真机会卡在控制器被 BIOS 抢占。

**SuperSpeed、hub 拓扑没做。** Cinux 只枚举 HID boot 鼠标 + 键盘 + tablet 的 interrupt-IN 端点。USB 3.0 的 5 Gb/s SuperSpeed、hub 的拓扑路由(Route String 非零)、isochronous/bulk 端点都不在路径上。

**MSI-X 只用了一个向量。** Table 里只编程了 entry 0(投递到 BSP)。HCSPARAMS1.MaxIntrs 多中断器、多向量负载分散、x2APIC 投递都没做。子系统本身支持多向量(将来给不同 interrupter 调 `program_vector(1, ...)` 即可),但本章只有一个消费者。

**AHCI 还没迁过来用 MSI-X。** MSI-X 子系统是为 xHCI 建的、预留了复用接口,但 AHCI 当前仍走传统的寄存器中断状态位,没迁。

验证该看到什么,见配套 lab。下一章 056 回到安全(NX/SMEP/SMAP + ASLR)——xHCI 这套 USB 输入先到这儿;后面那条「解耦收尾」会把 USB 在关掉时的空壳补齐,让 GUI 在没有真 xHCI 驱动时也能链接。
