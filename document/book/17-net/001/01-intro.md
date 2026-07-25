---
title: 01 · 导引:e1000 怎么点亮、怎么找出来
---

# 导引:e1000 怎么点亮、怎么找出来

> 要做网络(后面几章的以太网、ARP、IP、ping),第一步得有一块能收发包的网卡驱动。这一章给 QEMU 模拟的 **e1000**(Intel 8254x 系列,QEMU 默认模拟 82540em)写驱动:PCI 上把它找出来、映射它的寄存器、从 EEPROM 读出 MAC、立起收发环。这套活儿是 xHCI/AHCI 那些驱动走过无数遍的套路(PCI 枚举 + DmaPool + 轮询),e1000 是同一条跑道上再来一遍——所以重点不在「怎么写驱动」(那个 025/055 讲过),而在两个 e1000 特有的坑:**MMIO 寄存器窗口的槽位分配**(分一块地址给 e1000 而不撞别人),和**轮询收包时一个反直觉的细节**(在 QEMU 上,光读描述符内存永远收不到包,得读一个 MMIO 寄存器把模拟器「踢」一下)。验证靠真实流量——驱动把包发出去,QEMU 的 SLIRP 网关应答,驱动再把这个应答收回来(单播 ARP reply、广播 DHCP OFFER 都能收到)。这一步还没有 ping——ping 要等下一章把 IP/ICMP 立起来。
>
> 一条诚实的边界先说在前头:这个驱动是**轮询**的,不走中断。理由和 xHCI 一样——QEMU 在嵌套 KVM 下 MSI-X 锁存不可靠;而且 82540em 本来也只支持 MSI(不支持 MSI-X)。中断收包留到协议栈稳定后再说,这一章先把「能收能发」跑通。

## 这章咱们要点亮什么

1. **e1000 怎么被找出来**:按 vendor/device ID(Intel 0x8086 + e1000 设备号)匹配,**不是**按 class code 0x02——否则会把 virtio-net 也认成 e1000。
2. **MMIO 槽位分配**:内核有一段统一的 MMIO 窗口,AHCI/LAPIC/IOAPIC/xHCI/MSI-X 表各占一块;e1000 的寄存器得在这个窗口里分一块**不撞别人**的槽位——这是本仓一个致命的坑(BAR 地址撞了会直接崩)。
3. **从 EEPROM 读 MAC**:e1000 的 MAC 不在固定寄存器里,得分三次读 EEPROM、每次有界轮询等「读完了」位、再把三个 word 拼成六个字节。
4. **轮询收包那个反直觉的坑**:在 QEMU 上,光轮询内存里的描述符状态永远收不到包——得读一个 MMIO 寄存器,把模拟器的设备主循环「踢」一下,它才把线上的包推进 ring。

## 检测:按 vendor/device,不按 class

e1000 是挂在 PCI 总线上的设备,找它的路子和 xHCI 一样:扫 PCI 总线,挑出匹配的。但「匹配」这一步有个讲究——按 **vendor/device ID** 配,不按 class code:

```cpp
constexpr bool is_e1000_device(uint16_t vendor, uint16_t device) {
    if (vendor != 0x8086) {        // 0x8086 = Intel
        return false;
    }
    switch (device) {
    case 0x100E:  // 82540em (QEMU -device e1000 default)
    ...
    }
}
```

（`pci.hpp:71`,`find_e1000` 在 `pci.cpp:204`。）为什么不按 class 0x02(网络设备)匹配?因为 QEMU 里可能同时挂了 virtio-net,它也是 class 0x02。按 class 匹配会把 virtio-net 错认成 e1000,而两者的寄存器布局完全不同,一操作就崩。按 vendor/device 配就精准——0x8086:0x100E 只可能是 e1000(82540em)。这是个「宁可写死、别贪方便」的取舍:写 device ID 表罗嗦,但避开了误绑。

找到之后,`init()` 走一套固定的 PCI 设备启用流程:

```cpp
// 1. Enable PCI Bus Master (DMA) + Memory Space (MMIO response).
cmd |= PciCmd::BUS_MASTER | PciCmd::MEM_SPACE;
// 2. Map BAR0 (32-bit MMIO) into the MMIO window.
// 3. Software reset (CTRL.RST self-clears).  Bounded poll; non-fatal.
// 4. Disable interrupts (IMC), set link up (CTRL.SLU).
// 5. Read EEPROM -> MAC.
```

（`e1000.cpp:96` 起。）BUS_MASTER 让设备能做 DMA(收发包要用),MEM_SPACE 让它的 MMIO 寄存器能响应。这两位不设,后面一切都不动。

## MMIO 槽位:e1000 的寄存器放哪

e1000 的寄存器是一大块 MMIO(它的控制/状态寄存器、收发环的 head/tail、MAC 地址寄存器全在这块里,散在偏移 0x0 到 0x5404)。内核得把这块 MMIO 映射进自己的地址空间。问题是:**内核的 MMIO 窗口里已经挤了一堆别的设备**,e1000 得找一块没人用的槽位。

内核有一段统一的 `KMEM_MMIO` 窗口,各设备按偏移瓜分:

```cpp
// MMIO sub-allocation inside the KMEM_MMIO window.  In use: AHCI @+0x0, LAPIC
// @+0x10000, IOAPIC @+0x11000, xHCI @+0x20000, MSI-X Table @+0x40000, PBA
// @+0x41000.  e1000 BAR0 (32-bit MMIO) slots at +0x50000 -- clear of the MSI-X
// table ...
constexpr uint64_t kE1000MmioVirt = cinux::arch::KMEM_MMIO_BASE + 0x50000;
```

（`e1000.cpp:42`/`:47`。）e1000 用到 ~0x5404(收发环 + MAC 地址寄存器),得至少 6 页,取了 8 页留余量。`+0x50000` 是**逐个 grep 确认过没碰撞**的槽位——这块是本仓一个致命的坑:xHCI/MSI-X 当年就因为 BAR 地址撞了别的设备直接崩过。所以新设备进 MMIO 窗口,不能拍脑袋选个地址,得先把已占的偏移列出来、选一块空隙,再留够页数(初版这里就写过「4 页」不够覆盖 MAC 地址寄存器、被揪出来的错)。这一步看着琐碎,但漏了就是玄学崩。

## 从 EEPROM 读 MAC

e1000 的 MAC 地址不在某个固定寄存器里,存在 EEPROM(电可擦可编程只读存储器,一片独立的非易失存储)。读它得走一套「发起读 → 等完成 → 取数据」的协议,在 `EERD` 寄存器上完成:

```cpp
reg_write(e1000reg::EERD,
          (static_cast<uint32_t>(addr) << e1000reg::EERD_ADDR_SHIFT) | e1000reg::EERD_START);
// 有界轮询 DONE 位
if (reg_read(e1000reg::EERD) & e1000reg::EERD_DONE) {
    return (reg_read(e1000reg::EERD) >> e1000reg::EERD_DATA_SHIFT) & 0xFFFF;
}
```

（`e1000.cpp:64` 起。）一次读一个 16-bit word。QEMU 的 8254x 把地址放在 `EERD` 的 bit 11:8、数据放在 bit 31:16、START 是 bit 0、DONE 是 bit 4。写 START 发起一次读、轮询 DONE 等它读完(QEMU 上基本即时,但有界轮询防挂),从数据位取出 16-bit。MAC 是 6 字节,读 word 0/1/2 三个,按下标拼:

```cpp
mac_[0] = w0 & 0xFF;  mac_[1] = (w0 >> 8) & 0xFF;
mac_[2] = w1 & 0xFF;  mac_[3] = (w1 >> 8) & 0xFF;
mac_[4] = w2 & 0xFF;  mac_[5] = (w2 >> 8) & 0xFF;
```

（`e1000.cpp:82`。）QEMU user-net 给的默认 MAC 是 `52:54:00:12:34:56`——测试里读到这个值,就证明从 EEPROM 取 MAC 这条路通了。
