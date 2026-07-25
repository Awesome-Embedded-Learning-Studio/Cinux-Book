---
title: 03 · PCI:配置空间与揪出 AHCI
---

# PCI:配置空间与揪出 AHCI

## 代码路线

### PCI:配置空间那对 0xCF8/0xCFC 口

PC 上的 PCI 配置访问走「配置机制 #1」:一对 I/O 口,`0xCF8` 写地址,`0xCFC` 读写数据。这套口在 [pci_config.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/pci/pci_config.hpp) 里定成常量:

```cpp
namespace PciPort {
constexpr uint16_t CONFIG_ADDRESS = 0xCF8;  // 配置地址口
constexpr uint16_t CONFIG_DATA    = 0xCFC;  // 配置数据口
}
```

读一个配置寄存器,就是先把「总线/槽/功能/寄存器」编码成一个 32 位地址字写进 `0xCF8`,再从 `0xCFC` 读回 32 位数据。看 [pci.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/pci/pci.cpp):

```cpp
uint32_t PCI::pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (1U << 31)                          // bit31:使能
                     | (static_cast<uint32_t>(bus) << 16)  // bus   [23:16]
                     | (static_cast<uint32_t>(slot) << 11) // slot  [15:11]
                     | (static_cast<uint32_t>(func) << 8)  // func  [10:8]
                     | (offset & 0xFC);                    // reg   [7:2],dword 对齐
    io_outl(PciPort::CONFIG_ADDRESS, address);
    return io_inl(PciPort::CONFIG_DATA);
}
```

几个要点。bit 31 是「使能位」,不置这一位,这对口不认你写的东西。中间三段把总线、设备槽、功能号各放到自己的位域上——一个 PCI 槽可能挂多个「功能」(比如一块卡既是显卡又是声卡),所以有 bus/slot/func 三级定位。末尾 `offset & 0xFC` 是关键:配置空间按 32 位(dword)编址,一次读写一个 dword,所以寄存器偏移必须按 4 字节对齐,低 2 位清零。`0xFC` 就是「保留高 6 位、清掉低 2 位」。`pci_write` 用完全一样的地址字,只是数据方向反过来。

### 从茫茫总线里揪出 AHCI

知道怎么读寄存器了,枚举就是暴力扫:`32 bus × 32 slot × 8 func` 全试一遍,看每个位置有没有设备。判断「有没有」靠 vendor ID——读到 `0xFFFF` 表示这个位置空。`scan_function` 把一个位置的字段一次性解码出来:

```cpp
bool PCI::scan_function(uint8_t bus, uint8_t slot, uint8_t func, PCIDevice& dev) {
    uint32_t vendev = pci_read(bus, slot, func, PciReg::VENDOR_ID);  // offset 0x00
    uint16_t vendor = static_cast<uint16_t>(vendev & 0xFFFF);
    if (vendor == VENDOR_INVALID) return false;                       // 0xFFFF = 空

    uint32_t class_rev = pci_read(bus, slot, func, 0x08);             // 一个 dword 里塞了 class/subclass/prog_if
    dev.class_code = static_cast<uint8_t>((class_rev >> 24) & 0xFF);  // 最高字节
    dev.subclass   = static_cast<uint8_t>((class_rev >> 16) & 0xFF);
    dev.prog_if    = static_cast<uint8_t>((class_rev >> 8) & 0xFF);
    // ... vendor/device/header_type 同样从对应 dword 里抠 ...
    return true;
}
```

这里有个 PCI 配置空间的小聪明:它把好几个相关字段塞在**同一个 dword** 里。offset `0x00` 那个 dword 的低 16 位是 vendor、高 16 位是 device;offset `0x08` 那个 dword 里 class/subclass/prog_if/revision 各占一字节。所以读一次 `pci_read` 拿到的 32 位,按字节拆开就是多个字段——别误以为一个寄存器只放一个值。

`init()` 把这个扫描跑一遍并打印清单;`find_ahci` 扫描时多一个匹配条件:

```cpp
if (dev.class_code == PciClass::MASS_STORAGE &&   // 0x01 大容量存储
    dev.subclass == PciClass::AHCI_SUBCLASS) {    // 0x06 SATA / AHCI
    read_bars(dev);
    out = dev;
    return true;
}
```

`read_bars` 把设备的六个 BAR 全读出来,还要处理一个细节——BAR 有 I/O 型和内存型,内存型又分 32 位和 64 位:

```cpp
uint32_t raw = pci_read(dev.bus, dev.slot, dev.func, bar_offsets[i]);
if ((raw & BAR_IO_SPACE) != 0) {            // bit0==1: I/O 空间 BAR
    dev.bar[i] = raw & 0xFFFFFFFC;
} else {                                    // 内存空间 BAR
    dev.bar[i] = raw & BAR_ADDR_MASK_32;    // 0xFFFFFFF0,清掉低位 flag
    if ((raw & BAR_TYPE_MASK) == BAR_TYPE_64 && (i + 1) < BAR_COUNT) {
        // 64 位内存 BAR:下一个 BAR 寄存器是高 32 位地址
        uint32_t high = pci_read(dev.bus, dev.slot, dev.func, bar_offsets[i + 1]);
        dev.bar[i] = (static_cast<uint64_t>(high) << 32) | (raw & BAR_ADDR_MASK_32);
        dev.bar[i + 1] = 0;
        ++i;   // 消耗掉了下一个 BAR 槽位
    }
}
```

为什么 BAR 要这么费劲地区分。因为 BAR 寄存器的低位不是地址,而是 flag:bit 0 标 I/O 还是内存;内存型的 bit[2:1] 标类型(`0b00`=32 位、`0b10`=64 位)。64 位 BAR 要**两个**寄存器拼成一个地址,所以读到 64 位 BAR 时,得把下一个 BAR 槽位也吃掉、跳过 `++i`,否则会把高位地址当成独立的 BAR。AHCI 控制器的 ABAR(我们关心的 BAR5)在 QEMU 这套环境里是个 32 位内存 BAR,低 4 位是 flag、高位是物理基址,所以 `raw & 0xFFFFFFF0` 一抠就拿到地址。
