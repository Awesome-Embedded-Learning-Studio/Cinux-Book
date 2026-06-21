---
title: Lab 025 · 让内核自己找到磁盘:PCI 枚举 + AHCI 扇区读写
---

# Lab 025 · 让内核自己找到磁盘:PCI 枚举 + AHCI 扇区读写

> 配套章节:[025 · 让内核自己找到磁盘:PCI 枚举与 AHCI 驱动](../../book/08-filesystem/025-driver-ahci.md)。这一关给你目标和约束,不贴 CFIS 构造、不贴端口停起顺序的实现,也不贴命令提交的轮询。

## 实验目标

让内核第一次自己认设备、自己读写磁盘。拆成四个能逐个验证的子目标:

1. 能读 PCI 配置空间:通过 `0xCF8/0xCFC` 这对口,读出任意 bus/slot/func 的配置寄存器。
2. 能枚举并定位 AHCI:扫总线,从一堆设备里揪出 `class=0x01 && subclass=0x06` 的那块,读出它的 BAR5。
3. 能把控制器初始化起来:映射 BAR5、复位、给有设备的端口搭好命令列表和 FIS 缓冲。
4. 能发一条 DMA 读命令:构造 FIS + PRDT,提交命令、轮询完成,读出扇区 0 的 MBR 签名 `55 AA`。

做完这四条,内核就够得到磁盘了,后面文件系统的所有上层抽象才有地基。

## 前置条件

你得先过 Lab 016:VMM 的 `map(virt, phys, flags)` 可用——AHCI 的寄存器是内存映射的,读 BAR5 之前必须先把它 `map` 进虚拟地址空间。这一关所有「给 MMIO/命令缓冲映射虚拟地址」都转成对 VMM 的 `map` 调用,而且得知道 `FLAG_PCD` 这个 flag 的存在(MMIO 不能被缓存)。

PMM(Lab 015)的 `alloc_page` / `alloc_pages` 也要可用——命令列表、FIS 缓冲、DMA 目标缓冲都靠它提供物理连续、页对齐的页。

最好对 x86 I/O 端口指令(`in`/`out` 的 32 位版本)有点概念:PCI 配置访问是端口 I/O,和 VMM 那套内存映射是两条路。

## 任务分解

**第一步:理清 PCI 配置机制 #1。** 这是基础。配置访问靠一对 I/O 口:`0xCF8` 写地址、`0xCFC` 读写数据。地址字要自己拼:bit31 是使能位,后面依次是 bus、slot、func、寄存器偏移。两个坑要想清楚——为什么偏移要 `& 0xFC`(配置空间按 dword 编址,低 2 位是寄存器内字节偏移,一次读写一个 dword,得清零);为什么 bit31 必须置(不置这对口不响应)。把 `pci_read`/`pci_write` 写出来,先用它读某个已知设备的 vendor ID 验证通路。

**第二步:枚举 + 找 AHCI。** 暴力扫 `32 bus × 32 slot × 8 func`。每个位置先读 vendor ID,`0xFFFF` 表示空(且 func0 空通常意味着整个 slot 空,可以跳过后续 func)。非空就把 vendor/device/class/subclass/prog_if 解码出来——注意配置空间的字段是「挤」在 dword 里的:offset `0x00` 低 16 位是 vendor、高 16 位是 device;offset `0x08` 那个 dword 里 class/subclass/prog_if 各占一字节,读一次按字节拆。然后用 `class==0x01 && subclass==0x06` 筛出 AHCI。

**第三步:解码 BAR。** `find_ahci` 命中后,要把 BAR5 读出来当 AHCI 寄存器块的物理基址(叫 ABAR)。BAR 解码有讲究:bit0 区分 I/O(`1`)还是内存(`0`);内存型的 bit[2:1] 区分 32 位(`0b00`)和 64 位(`0b10`)。读到 64 位 BAR 时,**下一个 BAR 槽位是它的高 32 位**,得一起拼、并把下一个索引跳过——别把高位地址当独立 BAR。AHCI 的 ABAR 在 QEMU 环境下是 32 位内存 BAR,低 4 位 flag 清掉就是物理基址。

**第四步:初始化控制器。** 拿到 BAR5 后:先往 PCI 的 COMMAND 寄存器写两位(Bus Master + Memory Space,不开命令发不动、MMIO 读不到);再用 VMM `map` 把 BAR5 映射进来——**flag 必须带 `FLAG_PCD`**,想清楚为什么(MMIO 寄存器被 CPU 缓存会怎样)。然后是固定仪式:置 `GHC.AE` 进 AHCI 模式 → 置 `GHC.HR` 复位并等它清零 → 复位会清掉 AE,得重置一次 → 置 `GHC.IE` 开中断。最后按 `pi` 位图逐端口看 `PxSSTS` 的 DET 字段,`0x03` 才是有设备且链路通。

**第五步:搭端口命令队列。** 对有设备的端口,先停引擎,再分配两块物理连续、对齐的内存:**命令列表**(32 个命令头 × 32 字节)和 **FIS 接收缓冲**(256 字节),都要清零,把它们的物理地址写进端口的 `clb/clbu/fb/fbu`。然后给 slot 0 的命令头指向一张命令表(可以放在命令列表那页的后半段)。最后重新启动引擎。这步最绕的是停起顺序和「怎么访问那块物理内存」——停机要 ST→等 CR→FRE→等 FR,起机要 FRE→ST;清零/写结构那块物理内存,得靠「物理地址 + 高半区偏移」直访(前提是那段物理内存做了高半区映射,见 Lab 016)。

**第六步:发读命令。** 在 slot 0 上填一张 Register H2D FIS(`type=0x27`),命令码 `READ DMA EXT`(0x25),把 48 位 LBA 拆进 lba0-2(低 24 位)和 lba3-5(高 24 位),device 寄存器置 LBA 模式位,扇区数填 count0/count1。再填一个 PRDT 条目指向你的目标缓冲(地址 dba/dbau,长度 dbc 是「字节数-1」)。命令头的 cfl 填 FIS 长度的 dword 数,prdtl 填 PRDT 条数。清掉端口中断状态后,往 `port.ci` 写 `1<<0` 发命令,轮询这一位被硬件清零,再看 `tfd` 的 ERR 位确认没出错。成功的话,数据已经在你给的目标缓冲里了。

## 接口约束

你要实现出来的东西,对外长这样(职责和签名,不给实现):

- `PCI::pci_read(bus, slot, func, offset) -> uint32_t`:拼地址字写 `0xCF8`,从 `0xCFC` 读。
- `PCI::pci_write(bus, slot, func, offset, value)`:同上,数据写 `0xCFC`。
- `PCI::init()`:扫总线,打印设备清单。
- `PCI::find_ahci(PCIDevice& out) -> bool`:扫到 class=0x01 subclass=0x06 的,读 BAR 后填进 `out`,返回 true。
- `AHCI::init(const PCIDevice& dev)`:开 Bus Master/Memory Space、映射 BAR5、复位、起端口。
- `AHCI::read(port, lba, count, buf_phys) -> bool` / `write(...)`:在 slot 0 发一条 DMA 命令,轮询完成。

硬约束:

- `read`/`write` 的 `buf` 是**物理地址**,缓冲必须**物理连续**且页对齐(一整页天然满足)——DMA 靠 PRDT 直接搬物理内存,不连续/不对齐会越界。调用方负责用 PMM 分配、用 VMM 映射出虚拟地址去读内容。
- 只用 **slot 0、单命令串行**,不做命令并发;用非排队 DMA(READ/WRITE DMA EXT),**不是 NCQ**。
- BAR5 映射的 flag 必须含 `FLAG_PCD`;改端口 `clb/fb` 前必须停引擎;停起顺序不能反。

PCI 寄存器偏移、BAR 掩码、class 码、AHCI 各位常量、`HBAMem/HBAPort` 的字段布局,都得你照规范定(或直接读硬件),这关不提供布局答案。

## 验证步骤

先造测试盘。用 `dd` 做一张 1MB 全零盘,在偏移 510、511 处各写一字节 `0x55`、`0xAA`(就是 MBR 引导签名):

```bash
bash scripts/create_ahci_test_disk.sh build/ahci_test.img
```

QEMU 这边给测试目标挂一块 AHCI 总线 + 这张盘(`-device ahci,id=ahci` + `-drive file=ahci_test.img,if=none,id=ahci-disk` + `-device ide-hd,drive=ahci-disk,bus=ahci.0`)。这套 `cmake/qemu.cmake` 已经配好,不用手改。

纯逻辑(结构体大小、常量、FIS 的 LBA/count 编码、PCI 地址字、BAR 类型判别)在 host 上镜像测,`-O2` 编、`CINUX_HOST_TEST` 门控——因为内核实现全是 MMIO + 真物理内存,host 上没法直接调,只能测「同样的算法对不对」:

```bash
ctest --test-dir build -R ahci --output-on-failure
```

真寄存器、真 DMA,在 QEMU 里跑机内集成测试(三步:找到 AHCI、init 后 `hba_mem()` 非空、读扇区 0 验 `55 AA`):

```bash
cmake --build build --target run-kernel-test
```

跑完整内核看启动日志,会打一串 `[PCI]`、`[AHCI]` 行,验收点是最后这句:

```text
[AHCI] Read sector 0: 55 AA
```

## 常见故障

- **PCI 能认到 AHCI、BAR5 地址也对,但一碰寄存器全是 0/FF**:PCI 的 Memory Space 位没开。`init` 第一件事是往 COMMAND 寄存器写 Memory Space(bit2);DMA 不动则补 Bus Master(bit1)。
- **`port->ci` 写了之后死活不清零,轮询必超时**:BAR5 映射漏了 `FLAG_PCD`,MMIO 被 CPU 缓存,读到的是旧值。映射设备寄存器 flag 必带 PCD。
- **`setup_port` 看着成功、`ci` 也写了,命令就是不执行**:端口停起顺序写反。停机要 ST→等 CR→FRE→等 FR;起机要 FRE→ST。改 `clb/fb` 前没停引擎也会这样(引擎在跑,地址换了等于换地基)。
- **DMA 完成了但数据是乱的 / 越界**:`buf` 不物理连续,或 PRDT 的 `dbc` 写成了「字节数」而不是「字节数-1」。dbc 是 0 表示传 1 字节。
- **读出扇区 0 但 510/511 不是 55 AA**:要么命令方向/扇区号不对(LBA 没拆对、device 寄存器没置 LBA 模式位),要么 CI 清了但 `tfd.ERR` 置位你没检查就当成功了。轮询完务必看 ERR。
- **改命令列表/清零那段物理内存就重启**:用 `phys + 高半区偏移` 直访时,那页物理内存没做高半区映射,缺页。PMM 给的页要落在已映射区段(boot 期全映射了,一般不发作,但要知道前提)。

## 通过标准

1. host 单测全绿:结构体大小焊死(`HBAPort` `0x80`、命令头 `32`、PRD `16`、FIS `20`)、常量位定义、CFIS 的 48 位 LBA 与 count 编码、PCI 地址字、BAR 的 IO/32/64 判别。
2. QEMU 机内测三步通过:找到 AHCI(class=0x01 subclass=0x06 且 BAR5 非 0)、init 后 `hba_mem()` 非空、读扇区 0 返回 `buf[510]==0x55 && buf[511]==0xAA`。
3. BAR5 映射带 `FLAG_PCD`;端口改 `clb/fb` 前停引擎;停起顺序(ST→CR→FRE→FR / FRE→ST)不反。
4. `read`/`write` 用 slot 0 单命令、非排队 DMA;目标缓冲物理连续且对齐;轮询 CI 清零后查 `tfd.ERR`。

做到这四条,内核就第一次自己找到了磁盘、读出了扇区。但读出来的是裸 512 字节,还没有「文件」的概念——下一关,我们把一张 initrd 镜像当内存盘,在扇区上叠一层最小的文件抽象。
