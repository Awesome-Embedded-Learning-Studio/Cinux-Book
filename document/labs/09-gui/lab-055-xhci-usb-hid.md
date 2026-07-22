---
title: Lab 055 · xHCI USB HID 驱动验证
---

# Lab 055 · xHCI USB HID 驱动验证

> 对应 `document/book/09-gui/055-xhci-usb-hid.md`。验证档 **A 档**(用户可见新能力:USB 键鼠输入),但本章是驱动而非用户程序,验证主要靠内核测试 `test_xhci`(找控制器、复位、给设备寻址、跑通 HID 鼠标)+ 一组 host 单测 + grep。本章给内核接上 xHCI USB 主控制器:PCI 发现 → MSI-X 中断 → TRB/环/cycle-bit → 设备枚举 → HID boot 报告 → 收进 054 的 GUI 事件队列。

## 目标

确认七件事:

1. xHCI 能用 PCI 三段式 `0x0C/0x03/0x30` 精确识别(`CINUX_USB=ON`);
2. 控制器四块寄存器(Capability/Operational/Runtime/Doorbell)+ 先 Halt 再 HCRST 的复位流程;
3. MSI-X 给 xHCI 挂了**一个**向量(entry 0 → IDT `0x40` → BSP);
4. TRB 16 字节 + 命令环 Link TRB 绕回翻 PCS + 事件环按 CCS 出队;
5. 枚举用 input context(DW0=Drop、DW1=Add),且 SET_ADDRESS 被 Address Device 命令替掉;
6. HID boot 三种固定布局:鼠标 4 字节 / 键盘 8 字节 / tablet 5 字节;find_boot_mouse/keyboard 靠 class/subclass/protocol 识别;生产指针是 tablet(绝对坐标);
7. `big_kernel_test` 构建 0 error + `run-kernel-test-xhci` 全绿(含 `test_xhci`,真跑不是 skip)。

## 步骤

### 1. USB 开关 + xHCI 识别

```bash
grep -n 'CINUX_USB' CMakeLists.txt | grep option
grep -n 'is_xhci_device\|XHCI_PROG_IF\|SERIAL_BUS' kernel/drivers/pci/pci.hpp kernel/drivers/pci/pci_config.hpp
```

`CINUX_USB` 应默认 ON(`CMakeLists.txt` 的 option)。`is_xhci_device`(`pci.hpp:58`)应是三段全查的纯 `constexpr` 谓词,常量 `0x0C/0x03/0x30` 在 `pci_config.hpp:57`。

> **思考**:为什么必须查 prog_if,不能只比 class+subclass?——见章节:四代 USB 主控制器(UHCI/OHCI/EHCI/xHCI)共用 class 0x0C + subclass 0x03,只有 prog_if(0x00/0x10/0x20/0x30)区分它们。少查一段会把老控制器一起误命中。这个谓词特意做成纯函数,就是为了 host 单测能把四种错误 prog_if 都试一遍。

### 2. 控制器复位 + 四块寄存器

```bash
grep -n 'BUS_MASTER\|MEM_SPACE\|usbcmd = 0\|kHcReset\|kControllerNotReady\|TimedOut' kernel/drivers/usb/xhci_controller.cpp
grep -n 'cap_length_version\|dboff\|rtsoff\|kBaseOffset' kernel/drivers/usb/xhci_registers.hpp
```

去看 `xhci_controller.cpp:75`(置 PCI BUS_MASTER|MEM_SPACE)、`:104`(先 `usbcmd=0` 等 HCH)、`:114`(再 HCRST 等 CNR 清)。寄存器四块的起点都从 Capability 的 CAPLENGTH/DBOFF/RTSOFF 算(`xhci_registers.hpp`)。确认复位是「先停再复位」——控制器在跑的时候不能复位。

### 3. MSI-X 单向量 → 0x40

```bash
grep -n 'program_vector\|kXhciIrqVector\|0x40' kernel/drivers/usb/xhci_controller.cpp kernel/drivers/usb/xhci_irq.hpp
grep -n '0xFEE00000' kernel/drivers/pci/msix.cpp
```

`xhci_controller.cpp:230` 应是 `program_vector(0, kXhciIrqVector, 0)`——只编程 entry 0、向量 `0x40`(`xhci_irq.hpp`)、目标 BSP(APIC ID 0)。`msix.cpp:79` 的 xAPIC 地址基址 `0xFEE00000`。`0x40` 选在空闲段(避开 0x20-0x2F PIC IRQ / 0x80 sigreturn / 0xE0 resched IPI / 0xFF LAPIC spurious)。

### 4. TRB + 环 + cycle bit

```bash
grep -n 'struct Trb\|kCycleBit\|kLink\|kEnableSlot\|kAddressDevice' kernel/drivers/usb/xhci_trb.hpp
grep -n 'write_link\|enqueue_ == slots_\|pcs_ = !pcs_\|cycle != ccs_' kernel/drivers/usb/xhci_ring.cpp
```

TRB 16 字节(`xhci_trb.hpp:21`,static_assert),cycle bit 在 control bit0。命令环 enqueue 到末尾时重写 Link TRB + 归零 + 翻 PCS(`xhci_ring.cpp:37`);事件环 dequeue 看到 `cycle != CCS` 就是空(`xhci_ring.cpp:64`)。host enqueue 完得敲 doorbell(`xhci_controller.cpp:269` 的 `doorbells_[0]`)。

### 5. 枚举:input context + Address Device

```bash
grep -n 'in\[0\]\|in\[1\]\|input_add_flag\|build_address_input' kernel/drivers/usb/xhci_slot.cpp
grep -n 'kSetAddress\|kAddressDevice\|kEnableSlot' kernel/drivers/usb/usb_request.hpp kernel/drivers/usb/usb_init.cpp
```

`xhci_slot.cpp:97-98` 应是 `in[0]=0`(Drop)、`in[1]=A0|A1`(Add slot+EP0)——注意 DW0=Drop、DW1=Add 的顺序(写反 QEMU 报 TRB Error)。`usb_request.hpp:53` 的 `kSetAddress=5` 应标注「xHCI 不发,控制器用 Address Device 命令寻址」;枚举入口 `usb_init.cpp:50` 的 `enumerate_port` 串起 Enable Slot → Address Device → 读描述符。

### 6. HID boot 协议(鼠标 / 键盘 / tablet 三种)

```bash
# 鼠标 4 字节 + tablet 5 字节(都在 mouse/hid.hpp)
grep -n 'decode_boot_mouse\|find_boot_mouse\|decode_tablet\|TabletReport' kernel/drivers/mouse/hid.hpp
# 键盘 8 字节(在同名的 keyboard/hid.hpp,不是 mouse/hid.hpp)
grep -n 'decode_boot_keyboard\|find_boot_keyboard\|HidKeyboardReport' kernel/drivers/keyboard/hid.hpp
```

去看三种固定布局:`decode_boot_mouse`(`mouse/hid.hpp:43`)解 4 字节(byte0 按钮 / int8 dx / int8 dy / wheel),`decode_tablet`(`mouse/hid.hpp:64`)解 5 字节(buttons + 16 位小端 X/Y,0..32767),`decode_boot_keyboard`(`keyboard/hid.hpp:46`)解 8 字节(modifier / reserved / 6 个 keycode)。`find_boot_mouse`/`find_boot_keyboard` 按 class 0x03/sub 0x01/proto 在配置描述符里找 boot 接口。注意:鼠标 dy 不取反(Y+ = 屏幕向下),和 PS/2 相反;**生产指针实际是 tablet**(绝对坐标,0..32767 线性映射到屏幕像素),相对鼠标 `decode_boot_mouse` 主要是 `test_hid_mouse` 验证解码本身用的。

### 7. 内核构建 + 测试(用 xHCI 专用 target)

```bash
cmake --build build --target big_kernel_test -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
cmake --build build --target run-kernel-test-xhci 2>&1 | grep -E 'xHCI|test_xhci|Tests:'
```

> ⚠️ **必须用 `run-kernel-test-xhci`,不是 `run-kernel-test`。** 默认的 `run-kernel-test` 不挂 `-device qemu-xhci`,`pci.find_xhci()` 返回 false,三个 xHCI 用例全走 skip 早退——它们仍会打印 `[PASS]`(skip 不增失败计数),但**什么都没验证**。`run-kernel-test-xhci`(`cmake/qemu.cmake`)才挂 `qemu-xhci + usb-kbd + usb-tablet`,能端到端跑通 find/reset/address-device/HID。
>
> **别** `cmake --build build`(ALL):host 单测有既有债(`test_ext2_inode_ops` mock 滞后 kernel ErrorOr)会让 ALL 失败,跟本章无关。用 `big_kernel_test` + `run-kernel-test-xhci`。

应 `build=0`、测试全绿。日志里能看到 `[PCI] xHCI found: ...`、`[xHCI] controller reset complete (halted, CNR clear)`、`[xHCI] command pipeline: cmd_completions=...`,以及 `=== xHCI ===` 段里 `test_xhci::test_find_and_reset` / `test_address_device` / `test_hid_mouse` 三个用例真跑(不是 skip)并 `[PASS]`。

> host 单测(`test/unit/test_xhci.cpp` / `test_msix.cpp` / `test_usb.cpp` / `test_hid.cpp` / `test_pci.cpp`)用 mock 配置空间/栈数组跑真驱动代码,验证环 cycle 轮转、MSI-X 能力解码、HID 报告解码这些**纯逻辑**——它们能脱离 QEMU 跑,是因为 `find_capability`/`decode_boot_mouse` 这些都被写成了接收函数指针/裸指针的纯函数(章节讲的「内核驱动可测性范式」)。

## 验收清单

- [ ] `CINUX_USB` 默认 ON;`is_xhci_device` 三段式 `0x0C/0x03/0x30`。
- [ ] 控制器先 Halt(`usbcmd=0` 等 HCH)再 HCRST(等 CNR 清);PCI 置 BUS_MASTER|MEM_SPACE。
- [ ] MSI-X 编程 entry 0 → 向量 `0x40` → BSP;xAPIC 地址基址 `0xFEE00000`。
- [ ] TRB 16B + cycle bit bit0;命令环 Link TRB 绕回翻 PCS、事件环按 CCS 出队;doorbell 通知控制器。
- [ ] input context DW0=Drop/DW1=Add(Address Device 只 Add slot+EP0);SET_ADDRESS 被 Address Device 命令替掉。
- [ ] HID boot 三种固定布局:鼠标 4B(dy 不取反,异于 PS/2)/ 键盘 8B / tablet 5B(0..32767 绝对坐标);生产指针是 tablet。
- [ ] `big_kernel_test` `build=0` + `run-kernel-test-xhci` 全绿(含 test_xhci 三用例真跑、控制器被发现 `[PCI] xHCI found`)。

## 别做这些

- **别**以为真中断在 QEMU 下真送达——IMAN.IE 在 QEMU+nested-KVM 下锁存不可靠,生产走 worker 线程轮询事件环;MSI-X 武装留真硬件。`test_xhci` 靠轮询验证机制,不依赖真中断。
- **别**把 input context 的 Drop/Add flag 写反——DW0=Drop、DW1=Add(spec + Linux + QEMU 三方一致),写反 QEMU 直接返回 TRB Error(校验 DW0==0 && DW1==0x3)。
- **别**把 64 位寄存器(CRCR/DCBAAP/ERSTBA/ERDP)一次写——xHCI MMIO 对非对齐 64 位写行为未定义,得拆 `_lo`/`_hi` 两次写。
- **别**指望 BIOS Legacy Support handoff 做了——QEMU 启动即 OS-owned 直接跳过;真机才需要走扩展能力协商所有权。
- **别** `cmake --build build`(ALL)验证本章——host 单测既有债会失败;用 `big_kernel_test` + `run-kernel-test-xhci`。
- **别**用默认的 `run-kernel-test` 验证 xHCI——它不挂 qemu-xhci,三个 xHCI 用例全走 skip 早退(打印 `[PASS]` 但没验证);必须用 `run-kernel-test-xhci`。
- **别**把 SuperSpeed/hub 拓扑当成已支持——Cinux 只枚举 HID boot 鼠标+键盘+tablet 的 interrupt-IN,5 Gb/hub 路由/isochronous/bulk 都不在路径上。
