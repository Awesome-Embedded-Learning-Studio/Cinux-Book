---
title: "02 · 把总线走一遍"
description: "PCI 配置空间用两个端口一问一答:0xCF8 写地址字,0xCFC 收数据。地址字把使能位、总线、槽位、功能、寄存号编进一格。32 条总线 32 个槽 8 个功能走一遍,vendor 全 1 即空位,0 号功能不开整槽跳过。七台设备进终身稳定表,点名按类查,不按座位查。"
chapter: 15
order: 2
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - storage
  - pci
---

# 把总线走一遍

开机的时候,内核对机器里的设备一无所知:内存多大,引导程序交了底。总线上插着什么,没人交底。PCI 总线就是插设备的那排槽位,模拟器给咱们插了硬盘的控制器、网卡、显卡、几座桥。谁坐在哪个槽上,是机器装配的时候定下的,内核唯一的办法,是自己把槽位挨个问一遍。问的对象一共 32 条总线、每条 32 个槽、每槽 8 个功能,合起来八千多个位置——听着吓人,其实每一次问询就是一次端口读写,整个走完也就一眨眼。

问的方式,是两个端口的一问一答。往 `0xCF8` 写一个地址字,说明您要问谁,再从 `0xCFC` 把四个字节读回来。地址字的编法,咱们拿位段的词法摆开:

```cpp
constexpr cinux::base::bit::BitRange kConfigBus{.low = 16, .width = 8};
constexpr cinux::base::bit::BitRange kConfigSlot{.low = 11, .width = 5};
constexpr cinux::base::bit::BitRange kConfigFunction{.low = 8, .width = 3};
constexpr cinux::base::bit::BitRange kConfigRegister{.low = 2, .width = 6};
```

最高一位(bit 31)是使能位,不点亮它,这趟访问就不算配置访问。下面依次是总线号(8 位)、槽位号(5 位)、功能号(3 位)。末尾那 6 位装的不是字节地址,是 dword 的序号——配置空间的寄存器按四个字节一批进出,您报个没对齐的字节偏移,它自动落到所在的那批上。宿主测试里有一案,拿 `(2, 19, 1, 0x3C)` 编出 `0x8002993C`,再把 `0x3F` 这样的怪偏移编回去,验的正是这个错位回落的脾气。

位置报对了,门里有应答,也有空房。咱们怎么分辨?读它的身份寄存器,vendor 号要是全 1(`0xFFFF`),里头就没人。还有一层整槽的门道:

```cpp
std::optional<unsigned long> PciBus::scan_slot(uint8_t bus, uint8_t slot) {
    if (!probe_location(bus, slot, 0)) {
        return 0;
    }
    unsigned long found = 0;
    for (uint8_t func = 0; func < kScanFunctions; ++func) {
        std::optional<PciDevice> device = probe_location(bus, slot, func);
        if (device) {
            if (!admit(*device)) {
                return std::nullopt;
            }
            ++found;
        }
    }
    return found;
}
```

一个槽的八个功能,0 号是当家的。咱们敲 0 号的门,没人应,这个槽就整个跳过——门都没有的屋子,没必要挨个敲另外七扇。0 号应答了,咱们才把八个功能逐个探一遍,应答的进表。

进了表之前,咱们还得多读几处,把它的底细记全。身份寄存器里是厂商号加型号号,一对号牌。分类寄存器里躺着它归哪一类。头类型寄存器说它后半截配置空间按什么版式摆。中断线寄存器记着固件给它留的中断路线——线号咱们收着不动,认盘挂中断的时候,全靠它。再往下是六条 BAR,设备自报的六个窗口地址,内存窗还是端口窗,咱们往后要用。全都是生字节,照原样收。类别这一栏值得多说一句:它是三个字节的词表,大类一码、子类一码、编程接口一码。咱们把大类里跟存储有关的几个写成了有名字的常量:大存储是 `0x01`,它底下的子类里,老 IDE 是 `0x01`,AHCI 是 `0x06`,NVMe 排在 `0x08`。新类型照样进表——表里存的是生字节,词表只管查询。

咱们走完一遍,面板上前排七行,本节的收成全在这儿:

```text
[kern] pci 0:0:0 8086:1237 bridge 6:0
[kern] pci 0:1:0 8086:7000 bridge 6:1
[kern] pci 0:1:1 8086:7010 mass storage 1:1
[kern] pci 0:1:3 8086:7113 bridge 6:80
[kern] pci 0:2:0 1234:1111 display 3:0
[kern] pci 0:3:0 8086:100E network 2:0
[kern] pci 0:4:0 8086:2922 mass storage 1:6
[kern] pci walk found 7 devices
```

咱们拿最后一行念一遍格式:0 号总线、4 号槽、0 号功能,厂商号 `8086`、型号号 `2922`,大类小类念作 mass storage 1:6,正是 AHCI 那个子类。0:4:0 上的 `8086:2922`,就是模拟器给咱们硬盘配的控制器。0:1:1 也写着 mass storage,子类却是 1:1,那是老 IDE 的控制器,咱们这一卷不跟它打交道。剩下的,几座桥、一块显卡、一张网卡,各自归位。

表是固定 32 座的槽表,装满就报数收工。从此七台设备在内核里住进了终身稳定的位置:表只在开机时填这一回,填完不改,拿出去的指针一辈子有效。要用设备,咱们按类点名——`find_by_class` 拿大类加子类去查,不拿槽位号查,槽位号跟着装配变,类别长在设备身上,换台机器也认得。点名点中之后,还有一道唤醒的手续要办:

```cpp
command = (command | kCommandBusMaster | kCommandMemorySpace) & ~kCommandIntDisable;
```

咱们一口气开三样:内存解码(设备应答自己窗口的访问)、总线主控(设备有权自己发起搬运,盘跟内存倒数据靠的就是它)、再把固件可能留着的关中断位摘掉。三样齐了,设备才算站到了岗位上。本节收尾还沾一件小事:配置访问走的是 32 位端口,io 层那一对 `Out32`、`In32`,就是为它添的。

窗也开了,设备也醒了。总线咱们也走完了,再往前就是 AHCI 自己的领地。
