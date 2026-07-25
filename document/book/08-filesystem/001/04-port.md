---
title: 04 · AHCI:从 BAR5 映射到发出读命令
---

# AHCI:从 BAR5 映射到发出读命令

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
