---
title: 025 · 让内核自己找到磁盘:PCI 枚举与 AHCI 驱动
---

# 025 · 让内核自己找到磁盘:PCI 枚举与 AHCI 驱动

> 到 024 为止,内核已经能把程序丢进 Ring 3、收发系统调用、跑一个能 `echo`/`help`/`clear` 的 shell 了。但这个 shell 再花哨,程序和数据还是被 bootloader 一股脑塞进内存、只读、用完即弃——内核从来没「自己」碰过磁盘。这一章开始补这个洞:我们要让内核先在 PCI 总线上**找出**那块 SATA 控制器,再驱动它的 AHCI 接口,最终读出磁盘第 0 号扇区的 MBR 签名(`55 AA`)。能自己读写扇区,是后面一切文件系统、initrd、用户程序加载的前提。

## 这一章我们要点亮什么

一件能力,一个验证。

能力是:内核**第一次主动去碰外部存储设备**。具体分两层——先写一个 PCI 驱动,它会遍历总线、读每个设备的「身份证」(厂商/设备/类别码),从茫茫设备里认出「这块是 AHCI SATA 控制器」;再写一个 AHCI 驱动,把控制器的寄存器窗口映射进内核、初始化它、给它的端口搭好命令队列,然后通过 DMA 发一条「读扇区」命令,把磁盘上的数据搬进内存。从这一章起,内核不再只活在 RAM 里,它够得到磁盘了。

验证是:读出磁盘第 0 号扇区,检查它的最后两字节是不是 `55 AA`——这是传统 MBR 的引导签名。我们要在 QEMU 里挂一张手造的测试盘,在偏移 510-511 处写好 `55 AA`,然后看内核打出 `[AHCI] Read sector 0: 55 AA`。看到这行,说明「找设备 → 映射寄存器 → 起端口 → 发命令 → 收数据」整条链路全通。

这两件事合起来,意味着内核从「设备是别人给我配好的」升级成了「我自己认设备、自己驱动设备」。这是存储子系统的第一块基石。

## 为什么现在需要它

为什么紧跟在 userland 之后。022 到 024 让内核有了用户态、系统调用、shell,看起来像个「能交互的系统」了。但所有用户程序——包括那个 shell 本身——都是 bootloader 在启动时一次性塞进来的,内核运行起来之后没法再从磁盘加载任何东西。要真做成一个能「跑程序、读文件」的系统,内核得能自己读盘。而要读盘,首先得**知道盘在哪**——在 PC 上,这就是 PCI 总线。

为什么先 PCI 再 AHCI。现代 PC 的设备不是直接挂在 CPU 上的,而是挂在 PCI(以及它的继任者 PCIe)总线上。每块 PCI 设备有一小段「配置空间」,内核靠它认设备、拿到设备寄存器的基地址(BAR)。我们想用的 SATA 磁盘控制器,其寄存器接口遵循 AHCI 标准(Advanced Host Controller Interface),而 AHCI 控制器的寄存器块就暴露在它的 BAR5 上。所以顺序是死的:先写 PCI 驱动认出 AHCI 控制器、拿到 BAR5 地址,再写 AHCI 驱动操作那块寄存器。没有 PCI 这一步,AHCI 无从谈起。

还有一笔技术上的账。AHCI 的寄存器是**内存映射**的(MMIO)——它不是端口 I/O,而是把寄存器「铺」在一段物理地址上,CPU 像访问内存一样读写。但内核只能访问**虚拟**地址,所以读 BAR5 之前,得先用 016 写好的 VMM 把这块物理地址 `map` 进内核的虚拟地址空间。这里还要踩一个关键 flag:`FLAG_PCD`(Page Cache Disable),MMIO 寄存器**绝不能被 CPU 缓存**——这个坑调试现场专门讲。所以这一章不是凭空冒出来的,它直接站在 016 的 VMM 肩膀上。

## 设计图

整条链路分成「找设备」和「驱动设备」两段。先看 PCI 怎么认设备:

```text
   PCI 配置机制 #1:一对 I/O 口
        写地址字 ──→ 0xCF8 (CONFIG_ADDRESS)
        读写数据 ──→ 0xCFC (CONFIG_DATA)

   地址字 = bit31 使能 | bus<<16 | slot<<11 | func<<8 | (offset & 0xFC)
            └─ offset & 0xFC:寄存器偏移按 dword 对齐(低 2 位清零)

   枚举(暴力扫描 32 bus × 32 slot × 8 func):
        vendev = read(VENDOR_ID)            ← 一次读到 vendor(低16)+device(高16)
        if vendor == 0xFFFF: 这个位置没设备  ← func0 空 → 整个 slot 跳过
        class/subclass/prog_if 在 offset 0x08 那个 dword 里

   find_ahci:
        遍历,命中 class==0x01(大容量存储) && subclass==0x06(SATA/AHCI)
        read_bars:BAR5 就是 AHCI 寄存器块的物理基址(ABAR)
```

AHCI 这边,核心是「把 BAR5 映射进来 → 复位 → 给每个有设备的端口搭命令队列 → 发命令轮询」:

```text
   BAR5(物理) ──VMM.map(FLAG_PCD)──→ 内核虚拟地址(HBAMem*)
        │
        ├─ 通用寄存器:cap / ghc(全局控制) / pi(端口实现位图) / vs(版本)
        └─ ports[] @ offset 0x100,每端口 0x80 字节
                ├─ clb/clbu : Command List 基址(物理)
                ├─ fb/fbu   : FIS 接收缓冲基址(物理)
                ├─ cmd      : ST/CR/FRE/FR(引擎开关与状态)
                ├─ ssts     : SATA 状态,DET 字段==3 表示「有设备且链路通」
                ├─ ci       : Command Issue,写 1<<slot 发命令,完成时硬件清
                └─ tfd      : Task File Data,bit0 是 ERR

   发一条读命令(slot 0):
        Command List[0] ──指向──→ Command Table
                                    ├─ cfis[]:Register H2D FIS(type 0x27)
                                    │         command=0x25(READ DMA EXT)
                                    │         48位 LBA 拆进 lba0-2 / lba3-5
                                    └─ prdt[]:一个 PRD 指向目标缓冲(dbc=字节数-1)

        port.ci = 1<<0            ← 出发
        轮询 port.ci 清零 → 查 tfd.ERR==0 → 成功,数据已在缓冲
```

两段链路的衔接点是 BAR5:PCI 把它读出来交给 AHCI,AHCI 把它映射成 `HBAMem*` 后才能碰任何寄存器。

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

### AHCI:把 BAR5 这块寄存器窗映射进来

拿到 BAR5 物理地址了,AHCI 的活从 `init` 开始。第一件事不是碰寄存器,而是先让 PCI 允许这块卡干两件事:当总线主设备(能发起 DMA)、暴露内存空间(允许 MMIO 访问)。这是往 PCI 的 COMMAND 寄存器写两位:

```cpp
uint32_t cmd_reg = pci::PCI::pci_read(dev.bus, dev.slot, dev.func, pci::PciReg::COMMAND);
cmd_reg |= (1U << 1);   // Bus Master Enable —— 不开,DMA 发不出去
cmd_reg |= (1U << 2);   // Memory Space Enable —— 不开,MMIO 访问全 0/FF
pci::PCI::pci_write(dev.bus, dev.slot, dev.func, pci::PciReg::COMMAND, cmd_reg);
```

这两位漏一个,BAR5 要么读不到、要么命令发不动——调试现场里它排第一号坑。

然后映射 BAR5。AHCI 的所有寄存器(全局控制、端口寄存器)都铺在 BAR5 这块物理地址上,内核要通过虚拟地址访问,所以用 016 的 VMM 把它 `map` 进来:

```cpp
HBAMem* AHCI::map_bar5(uint64_t bar5_phys) {
    constexpr uint32_t BAR5_PAGES = 2;   // 映 2 页,够盖到 8 个端口
    constexpr uint64_t mmio_flags = cinux::arch::FLAG_PRESENT
                                  | cinux::arch::FLAG_WRITABLE
                                  | cinux::arch::FLAG_PCD;   // ★ MMIO 必须 uncached
    for (uint32_t i = 0; i < BAR5_PAGES; ++i) {
        uint64_t phys = bar5_phys + i * cinux::arch::PAGE_SIZE;
        uint64_t virt = MMIO_VIRT_BASE + i * cinux::arch::PAGE_SIZE;  // 0xFFFF800000100000
        if (!cinux::mm::g_vmm.map(virt, phys, mmio_flags)) return nullptr;
    }
    return reinterpret_cast<HBAMem*>(MMIO_VIRT_BASE);
}
```

那个 `FLAG_PCD` 是这一章的命门。MMIO 寄存器是**设备**状态,不是普通内存,CPU 若把它缓存起来,你读 `port->ci`(命令完成位)读到的可能是缓存里的旧值、永远不清零;你写 `ghc` 可能只进了缓存、没到设备。`FLAG_PCD` 把这块映射标成「禁用缓存」,每次读写都直达设备。映射的虚拟地址 `MMIO_VIRT_BASE = 0xFFFF800000100000` 选在内核高半区(地址高位为全 1,是 64 位规范形地址),和内核自己的 `KERNEL_VMA(0xFFFFFFFF80000000)` 错开,避免和别的映射打架。

`HBAMem` 这块结构体([ahci_config.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/ahci/ahci_config.hpp))就是 BAR5 的布局:开头是全局寄存器(cap/ghc/is/pi/vs…),然后 `ports[]` 从 offset `0x100` 开始,每个端口 `0x80` 字节。它和 `HBAPort` 都标了 `[[gnu::packed]]`,并各有一条 `static_assert` 焊死大小——MMIO 布局差一个字节就全错,编译期必须卡住。

映射好之后是「复位 → 开 AHCI 模式 → 开中断」的固定仪式:

```cpp
hba_mem_->ghc |= GhcBits::AE;     // AHCI Enable,先切到 AHCI 模式
reset_hba();                      // GHC.HR 复位,等它自己清零
hba_mem_->ghc |= GhcBits::AE;     // 复位会把 AE 清掉,得重新置
hba_mem_->ghc |= GhcBits::INT_ENABLE;
```

`reset_hba` 置 `GHC.HR` 后死循环轮询它被硬件清零(硬件复位完会自己清)。注意复位**会把 `AE` 一起清掉**,所以复位后必须重新置 `AE`——这是规范要求,顺序反了或漏了,AHCI 模式没真正生效,后面端口全不听话。

### 端口起步:停机 → 分配 Command List/FIS → 起机

`init` 最后按 `pi`(Port Implemented,端口实现位图)逐个端口探测。每个端口先看 SATA 状态寄存器的 `DET` 字段,`0x03` 才表示「物理连了设备、链路已通」:

```cpp
uint32_t det = port->ssts & PxSsts::DET_MASK;   // 低 4 位
if (det != PxSsts::DET_ACTIVE) {                 // DET_ACTIVE = 0x03
    // 没设备,跳过
    continue;
}
setup_port(i);
```

有设备的端口,`setup_port` 给它搭命令队列。AHCI 的工作模型是:每个端口有一张**命令列表**(32 个命令头,每个 32 字节)和一个**FIS 接收缓冲**(256 字节,设备往这塞状态 FIS)。这俩得是物理连续、对齐的内存,内核要先从 PMM 要页、清零、把它们**物理**地址写进端口的 `clb/clbu/fb/fbu` 寄存器:

```cpp
void AHCI::setup_port(uint8_t port_index) {
    auto* port = &hba_mem_->ports[port_index];
    stop_port(port);                                  // ★ 改 clb/fb 前必须先停引擎

    uint64_t cmd_list_phys = cinux::mm::g_pmm.alloc_pages(1);  // 命令列表,4KB 页
    // ... 清零(靠 phys + KERNEL_VMA 直访)...
    uint64_t fis_buf_phys = cinux::mm::g_pmm.alloc_page();     // FIS 缓冲,1 页
    // ... 清零 ...

    port->clb  = static_cast<uint32_t>(cmd_list_phys & 0xFFFFFFFF);
    port->clbu = static_cast<uint32_t>(cmd_list_phys >> 32);   // 64 位地址拆高低
    port->fb   = static_cast<uint32_t>(fis_buf_phys & 0xFFFFFFFF);
    port->fbu  = static_cast<uint32_t>(fis_buf_phys >> 32);

    // 把 slot 0 的命令头指向命令表(放在命令列表那页的后半段)
    headers[0].ctba  = static_cast<uint32_t>(cmd_tbl_phys & 0xFFFFFFFF);
    headers[0].ctbau = static_cast<uint32_t>(cmd_tbl_phys >> 32);

    start_port(port);
}
```

这里有两个「为什么」。第一,改 `clb/fb` 这些基地址寄存器前**必须先停引擎**(`stop_port`):命令引擎若还在跑,你换了它脚下的命令列表地址,等于在行驶中换轮子,后果是数据错乱甚至控制器锁死。第二,内核要往命令列表页里写命令头、清零,得能访问这块**物理**内存——代码用 `cmd_list_phys + 0xFFFFFFFF80000000ULL`(也就是 `KERNEL_VMA`)直接当虚拟地址访问,这靠的就是 016 说过的「整段物理内存做了高半区恒等映射」那个 boot 期约定。

`stop_port` 和 `start_port` 的顺序是规范的硬要求,一个字都不能乱:

```cpp
void stop_port(HBAPort* port) {
    port->cmd &= ~PxCmd::ST;           // 先清 Start
    // 等 CR(Command Running)清零 —— 引擎真的停了
    port->cmd &= ~PxCmd::FRE;          // 再清 FIS Receive Enable
    // 等 FR(FIS Receive Running)清零
}
void start_port(HBAPort* port) {
    port->cmd |= PxCmd::FRE;           // 先开 FIS 接收
    port->cmd |= PxCmd::ST;            // 再开命令处理
}
```

停机要先停命令引擎(ST→等 CR)、再停 FIS 接收(FRE→等 FR);起机反过来,先开 FRE、再开 ST。顺序反了,控制器拒绝配合或直接卡住——这是 AHCI 规范里写死的端口控制顺序。

### 发一条读命令:CFIS + PRDT + 轮询 CI

端口搭好了,`read`/`write` 就是在 slot 0 上发一条命令。AHCI 命令分三块:命令头(在命令列表里,指向命令表)、命令表(放 FIS 和 PRDT)、FIS(告诉设备具体干嘛)。`execute_command` 把它们拼起来:

```cpp
// 命令表放在命令列表那页的 slot 0 之后(cmd_list + 32×32 字节)
cmd_tbl_phys = cmd_list_phys_[port_index] + CMD_SLOTS * sizeof(HBACommandHeader);
build_cfis(cmd_tbl, write_cmd, lba, count);   // 填 FIS

// PRDT:一个条目指向目标缓冲(物理连续)
cmd_tbl->prdt[0].dba  = static_cast<uint32_t>(buf_phys & 0xFFFFFFFF);
cmd_tbl->prdt[0].dbau = static_cast<uint32_t>(buf_phys >> 32);
cmd_tbl->prdt[0].dbc  = (count * SECTOR_SIZE - 1) & 0x3FFFFF;   // 字节数-1,22位
cmd_tbl->prdt[0].i    = 1;   // 完成时中断

// 命令头:CFIS 长度(以 dword 计)、PRDT 条数、方向
headers[slot].cfl   = sizeof(RegH2DFIS) / 4;   // 20 字节 = 5 dword
headers[slot].prdtl = 1;
headers[slot].write = write_cmd ? 1 : 0;

port->is = static_cast<uint32_t>(~0U);   // 清中断状态
port->ci = (1U << slot);                  // ★ 往 Command Issue 写位 = 发命令

for (uint32_t i = 0; i < POLL_TIMEOUT; ++i) {
    if ((port->ci & (1U << slot)) == 0) {  // 硬件完成会清掉这一位
        if ((port->tfd & 0x01) != 0) return false;   // TFD.ERR 有错
        return true;
    }
    __asm__ volatile("pause");
}
```

几个细节值得停一下。PRDT(Physical Region Descriptor Table)是 DMA 的散列-聚集表,一个条目描述一块物理连续缓冲:地址(dba/dbau)+ 长度(dbc)。注意 `dbc` 是「**字节数减一**」(`count * SECTOR_SIZE - 1`),这是硬件约定——0 表示传 1 字节。这里只用一个 PRD 条目,所以要求调用方给的缓冲必须**物理连续**(一整页天然连续,016 的 `alloc_page` 给的就是这个)。

FIS 本身是 `build_cfis` 填的 Register Host-to-Device FIS(`0x27`),里头是一条 ATA 命令:

```cpp
void build_cfis(HBACommandTable* cmd_tbl, bool write_cmd, uint64_t lba, uint16_t count) {
    auto* fis = reinterpret_cast<RegH2DFIS*>(cmd_tbl->cfis);
    fis->fis_type = FisType::REG_H2D;       // 0x27
    fis->flags    = 0x80;                   // 标志字节,见下文说明
    fis->command  = write_cmd ? AtaCmd::WRITE_DMA_EXT   // 0x35
                              : AtaCmd::READ_DMA_EXT;   // 0x25
    // 48 位 LBA:低 24 位进 lba0-2,高 24 位进 lba3-5
    fis->lba0 = lba & 0xFF;          fis->lba1 = (lba >> 8) & 0xFF;
    fis->lba2 = (lba >> 16) & 0xFF;  fis->device = 0x40;   // LBA 模式位
    fis->lba3 = (lba >> 24) & 0xFF;  fis->lba4 = (lba >> 32) & 0xFF;
    fis->lba5 = (lba >> 40) & 0xFF;
    fis->count0 = count & 0xFF;      fis->count1 = (count >> 8) & 0xFF;
}
```

这里得停下来挑一个细节:`flags = 0x80` 这个标志字节。Register H2D FIS 的字节 1,按 AHCI/Serial ATA 规范,bit 6 是 **C 位(command)**——置 1 表示「这是一条命令」,bit 7 是保留位。也就是说,规范意义上的「命令」应当把这一字节写成 `0x40`。而这章的代码写的是 `0x80`(bit 7),源码注释自己也写「bit 6 set = command」——注释和值对不上。它能跑通,是因为 QEMU 的 AHCI/IDE 模拟不强校验这个 C 位、照着 command 字段就处理了。这是个「在 QEMU 里宽容通过、未必符合规范字节布局」的点:本教程照实写当前 tag 的值 `0x80`,但你要知道规范那里要的是 bit 6。真机上若碰到控制器较真、命令不执行,这里是头号嫌疑。

`READ DMA EXT`/`WRITE DMA EXT`(`0x25`/`0x35`)是 48 位 LBA 的非排队 DMA 命令。48 位 LBA 的编码是 ATA 的老传统:LBA 拆成低 24 位(进 `lba0/1/2`)和高 24 位(进 `lba3/4/5`),设备位寄存器置 `0x40` 表示「LBA 模式」。注意这一章用的是**普通 DMA,不是 NCQ**(Native Command Queuing)——不排队、不并发,`count` 也只是 16 位扇区计数。

最后那个轮询是验收的关键:`ci`(Command Issue)写位发命令,**命令完成时硬件会自己清掉这一位**。所以死循环轮询 `port->ci` 这一位变 0,就表示命令做完了——但做完不等于做对,还得看 `tfd`(Task File Data)的 bit 0(`ERR`)是不是 0。只看 CI 清零不看 ERR,会把「命令失败」误判成成功。

得说清这一章的边界:代码只用了 **slot 0、单条命令串行**(`execute_command` 里 `cmd_tbl_phys` 固定指向 slot 0 的命令表,`slot` 参数实际被忽略)。AHCI 的命令列表能塞 32 条、理论上能并发,但这一章没那么干——够验证「能读盘」就行,并发提交是以后的事。

## 调试现场

025 这个 tag 没有独立的 notes 文件,但 PCI + AHCI 是典型「写错一个 flag 就整条链路静默失败」的硬件驱动,有几个坑值得当成调试现场(参 016 的先例:没 notes 也从代码隐患推导)。

一是 **PCI 的 Bus Master / Memory Space 没开**。`init` 里那两行 `cmd_reg |= (1<<1) | (1<<2)` 不是装饰。Bus Master 位不开,控制器不能发起 DMA,命令发出去石沉大海、`ci` 永不清零;Memory Space 位不开,MMIO 访问读到的是全 0 或全 FF,连 `pi` 位图都读不对。症状是「PCI 能认到 AHCI、BAR5 地址也对,但一碰寄存器全是垃圾」。根因就是忘了往 COMMAND 寄存器写这两位——很多教程默认 BIOS 已经开好了,但内核复位过 HBA 后得自己确保。

二是 **MMIO 漏了 `FLAG_PCD`**。`map_bar5` 的 flag 里 `FLAG_PCD` 是命门。漏了它,CPU 会把 BAR5 那块寄存器当普通内存缓存起来。于是你读 `port->ci` 读到的是进循环前缓存的「1」,死活等不到它清零,最后 POLL_TIMEOUT 超时;或者你以为发了复位命令,其实只进了 CPU 缓存、控制器压根没收到。这种「读到的值不随设备变化」、轮询必超时的症状,九成是 MMIO 被缓存了。规矩:凡是映射设备寄存器,flag 必带 `FLAG_PCD`(设备型内存不可缓存)。

三是 **端口停起顺序写反**。`stop_port` 必须「先 ST→等 CR 清→再 FRE→等 FR 清」,`start_port` 必须「先 FRE→再 ST」。这套顺序是 AHCI 规范规定的端口状态机要求。顺序反了(比如起机时先 ST 后 FRE),控制器会拒绝启动命令引擎,`setup_port` 看着成功了、`ci` 也写了,但命令永远不执行、`ci` 不清。改 `clb/fb` 前没停机就更糟——引擎还在跑,你换了它读命令的地址,数据写到不可预测的地方。

四是 **直接拿物理地址当虚拟地址访问的脆弱性**。命令列表页和 FIS 页,代码用 `phys + 0xFFFFFFFF80000000ULL`(KERNEL_VMA)直接清零、写命令头。这能工作,完全依赖「整段物理内存都做了高半区恒等映射」这个 boot 期约定(016 的 `phys_to_virt` 同理)。一旦以后缩小高半区映射范围,或某页物理内存落在没映射的区段,这行就会缺页——而此刻可能正在处理别的缺页,递归下去 double fault。所以心里要有数:这章对 PMM 给的页做「物理直访」,是借了高半区映射的光,不是放之四海皆准的做法。

## 验证

验证分两层,和 016 一样:纯逻辑在 host 上镜像测,真硬件交互在 QEMU 里跑。

host 单测 [test_ahci.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_ahci.cpp) 把能脱离硬件测的逻辑抄了一份——结构体大小(`HBAPort` 必须 `0x80`、`HBAMem` 减一个端口必须 `0x100`、命令头 `32`、PRD `16`、FIS `20`)、各类常量位定义、CFIS 的 48 位 LBA 与 count 编码、PCI 地址字构造、BAR 的 IO/32/64 判别、PRDT 字节数计算。因为 `ahci.cpp` 里全是 MMIO + 真物理内存操作,host 上没法直接调内核实现,所以测的是「同样的算法在 host 上对不对」:

```bash
ctest --test-dir build -R ahci --output-on-failure
```

「真寄存器、真 DMA」只有 QEMU 里验得了真。先把测试盘造出来——[create_ahci_test_disk.sh](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/scripts/create_ahci_test_disk.sh) 拿 `dd` 造一张 1MB 全零盘,在偏移 510、511 处写 `0x55`、`0xAA`。QEMU 那边 [qemu.cmake](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/cmake/qemu.cmake) 给测试目标挂上 `-device ahci,id=ahci -drive file=ahci_test.img,if=none,id=ahci-disk -device ide-hd,drive=ahci-disk,bus=ahci.0`,于是 QEMU 里就有一块挂在 AHCI 总线上的 SATA 盘。

机内集成测试 [test_ahci.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_ahci.cpp) 三步走:PCI 能找到 AHCI(`class==0x01 && subclass==0x06` 且 BAR5 非 0)、`AHCI::init` 后 `hba_mem()` 非空、读扇区 0 后 `buf[510]==0x55 && buf[511]==0xAA`。跑它:

```bash
cmake --build build --target run-kernel-test
```

或者直接跑完整内核看启动日志,会打出一串 `[PCI]`、`[AHCI]` 行,最后那句就是验收点:

```text
[AHCI] Read sector 0: 55 AA
```

看到 `55 AA`,说明从「枚举认设备」到「DMA 收数据」整条链路通了——这一章就成了。验证的难点和 016 类似:AHCI 的正确性没法直接「看」,只能靠「读出的数据对不对」间接验证,所以那批焊死布局与编码的 host 单测(卡住结构体/常量)+ 机内测(真跑一遍 DMA)缺一不可。

## 下一站

到这里,内核第一次能自己认出磁盘、自己读写扇区了。但你会发现一个抽象层的缺口:我们读出来的是**裸扇区**——512 字节一坨,没有「文件」、没有「目录」、没有名字。现在内核会读盘了,但还不知道盘上那一堆扇区该怎么组织成「文件」。

下一站,我们先从最简单的开始:把一张预先打包好的镜像(initrd)当内存盘,在扇区上叠一层最小的「文件」抽象——按某种布局读出「哪个文件在第几扇区、多长」。这能在没有完整文件系统、也不依赖磁盘写操作的前提下,让内核加载并运行磁盘上的程序。不过那是下一章的事,我们先把「内核能自己找到磁盘并读出扇区」这个里程碑坐实。

---

### 参考

- Intel AHCI Specification rev 1.3(`[ahci_config.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/ahci/ahci_config.hpp)` 头注释已引):HBA 寄存器布局(通用区到 `0x100`、每端口 `0x80`)、`pi` 端口位图、`GHC.AE/HR/IE`、`PxCmd.ST/CR/FRE/FR` 的端口状态机与停起顺序、`PxSSTS.DET`(`0x03`=设备在线)、Command List(32×32B)/Received FIS(256B)的尺寸与对齐、Register H2D FIS(`0x27`)字段、PRDT 的 dbc「字节数-1」约定。权威硬件依据。
- OSDev — [PCI](https://wiki.osdev.org/PCI):配置机制 #1(`0xCF8`/`0xCFC`、bit31 使能)、配置空间寄存器偏移、BAR 类型(IO/内存、32/64 位)解码、class code 表。
- OSDev — [AHCI](https://wiki.osdev.org/AHCI):ABAR=BAR5、命令提交与 CI 轮询的社区实现路线、端口初始化步骤,是这套驱动最直接的对照参考。
- 016 章 · [把物理页挂进虚拟地址:VMM](../05-memory/016-mm-vmm.md):BAR5 的 MMIO 映射、命令列表/FIS 页的「物理直访」,直接复用 016 的 `g_vmm.map`、`FLAG_PCD` 和 `phys + KERNEL_VMA` 高半区约定。
- 本 tag 源码:[pci.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/pci/pci.cpp) / [pci.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/pci/pci.hpp) / [pci_config.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/pci/pci_config.hpp)、[ahci.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/ahci/ahci.cpp) / [ahci.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/ahci/ahci.hpp) / [ahci_config.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/ahci/ahci_config.hpp)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp)(Step 20-22 集成);测试 [test_ahci.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_ahci.cpp)(host 镜像)、[test_ahci.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_ahci.cpp)(QEMU 真跑)、[create_ahci_test_disk.sh](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/scripts/create_ahci_test_disk.sh)(造测试盘)、[qemu.cmake](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/cmake/qemu.cmake)(挂 AHCI 盘)。
