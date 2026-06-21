---
title: 参考 · 存储:PCI 枚举与 AHCI 扇区读写
---

# 参考 · 存储:PCI 枚举与 AHCI 扇区读写

> 查阅层。这一页是 Cinux 存储子系统(PCI 设备枚举 + AHCI SATA 驱动)的速查表,不按 tag 组织,给后续章节(026 ramdisk 及以后的文件系统)查接口、寄存器位、命令流程用。实现细节见 [025 · 让内核自己找到磁盘](../book/08-filesystem/025-driver-ahci.md)。
>
> 范围:本文对应 `025_driver_ahci` 这个 tag 的能力——能找设备、能读/写**裸扇区**。不含文件系统、不含块缓存、不含 NCQ/多命令并发。

## 子系统地图

```text
   PCI 配置空间 (0xCF8/0xCFC)
        │  枚举 bus/slot/func,读 vendor/class/BAR
        ▼
   PCI::find_ahci  ──命中 class=0x01 subclass=0x06──▶  BAR5(ABAR,物理)
        │
        │  VMM.map(BAR5, FLAG_PCD)
        ▼
   HBAMem* (AHCI 寄存器窗,虚拟地址)
        │  reset → AE → IE → 按 pi 位图起端口
        ▼
   AHCI::read/write(扇区级 DMA)
        │  CFIS + PRDT → 写 port.ci → 轮询完成
        ▼
   裸 512B 扇区数据
```

调用方依次:`PCI::init` → `PCI::find_ahci` → `AHCI::init(dev)` → `AHCI::read(port,lba,count,buf_phys)`。`AHCI::init` 内部会自己 `map` BAR5、复位、起端口。

## PCI 配置机制 #1

| 项 | 值 |
|---|---|
| 配置地址口 | `0xCF8` |
| 配置数据口 | `0xCFC` |
| 地址字 bit31 | 使能位(必置) |
| 地址字 bus | `[23:16]` |
| 地址字 slot | `[15:11]`(0-31) |
| 地址字 func | `[10:8]`(0-7) |
| 地址字 register | `[7:2]`,即 `offset & 0xFC`(dword 对齐) |

读法:把地址字写进 `0xCF8`,从 `0xCFC` 读回 32 位。写法同理,数据写进 `0xCFC`。

常用寄存器偏移(Cinux `PciReg`):

| 寄存器 | 偏移 | 说明 |
|---|---|---|
| VENDOR_ID | `0x00`(低16) | `0xFFFF` = 空槽 |
| DEVICE_ID | `0x02`(高16,与 vendor 同 dword) | |
| COMMAND | `0x04` | bit1=Bus Master、bit2=Memory Space,驱动前要置 |
| REVID/PROG_IF/SUBCLASS/CLASS_CODE | `0x08` 一个 dword | class 在最高字节 |
| HEADER_TYPE | `0x0E` | |
| BAR0..BAR5 | `0x10..0x24`,步长 4 | AHCI 的 ABAR = BAR5 |

类别码:`MASS_STORAGE = 0x01`,`AHCI_SUBCLASS = 0x06`。

BAR 解码(`PCI::read_bars`):

- bit0 = 1 → I/O 空间 BAR,地址掩 `0xFFFFFFFC`。
- bit0 = 0 → 内存 BAR,地址掩 `0xFFFFFFF0`;bit[2:1] = `0b10`(即 `0x04`)为 64 位,此时下一个 BAR 槽位是高 32 位,需拼接并跳过该槽位。

枚举上限:`MAX_BUS=32`、`MAX_SLOT=32`、`MAX_FUNC=8`(本驱动暴力全扫)。

## AHCI 寄存器布局

ABAR(BAR5)映射成 `HBAMem`(`[[gnu::packed]]`),字段偏移如下:

| 字段 | 偏移 | 说明 |
|---|---|---|
| cap | `0x00` | Host Capabilities |
| ghc | `0x04` | Global HBA Control |
| is | `0x08` | Interrupt Status(端口位图) |
| pi | `0x0C` | Port Implemented(端口实现位图) |
| vs | `0x10` | AHCI Version |
| … | `0x14-0x28` | ccc_ctl/em_loc/cap2/bohc 等 |
| ports[] | `0x100+` | 每端口 `0x80` 字节 |

`static_assert(sizeof(HBAMem) - sizeof(HBAPort) == 0x100)` 保证端口从 `0x100` 开始。

`HBAPort`(每端口 `0x80` 字节)关键字段:

| 字段 | 偏移 | 用途 |
|---|---|---|
| clb / clbu | `0x00 / 0x04` | Command List 基址(低/高 32 位) |
| fb / fbu | `0x08 / 0x0C` | FIS 接收缓冲基址(低/高 32 位) |
| is / ie | `0x10 / 0x14` | 中断状态 / 使能 |
| cmd | `0x18` | 端口命令与状态(ST/CR/FRE/FR 等) |
| tfd | `0x20` | Task File Data,bit0 = ERR |
| sig | `0x24` | 设备签名(SATA = `0x00000101`) |
| ssts | `0x28` | SATA 状态,低 4 位 DET |
| ci | `0x38` | Command Issue,写 `1<<slot` 发命令,完成硬件清 |

## 关键位常量

`GhcBits`(ghc 寄存器):

- `AE = 1<<31`:AHCI Enable。
- `INT_ENABLE = 1<<1`。
- `HBA_RESET = 1<<0`:复位,硬件完成后自己清零(复位会同时清 AE)。

`PxCmd`(cmd 寄存器):

- `ST = 1<<0`:Start;`CR = 1<<15`:Command Running(ST 清后硬件清)。
- `FRE = 1<<4`:FIS Receive Enable;`FR = 1<<14`:FIS Receive Running。

`PxSsts.DET`:`0x03` = 设备在线且链路通信正常(`DET_ACTIVE`)。

## 端口停起顺序(规范硬要求)

```text
   停机: cmd &= ~ST  → 等 CR 清零 → cmd &= ~FRE → 等 FR 清零
   起机: cmd |= FRE  → cmd |= ST
```

改 `clb/fb` 这些基地址寄存器前**必须先停引擎**;顺序反了控制器会拒绝配合或锁死。

## 命令提交(slot 0、非排队)

内存结构(都需物理连续、对齐、清零):

| 结构 | 大小 | 对齐 |
|---|---|---|
| Command List | 32 头 × 32B = 1024B | 1KB |
| Received FIS Buffer | 256B | 256B |
| Command Table | 头 128B + PRDT(≤8×16B) | 128B |
| HBACommandHeader | 32B | 列表里按槽位排 |
| HBAPrdtEntry | 16B | 4 字节 |

命令头(`HBACommandHeader`)关键位:`cfl`(FIS 长度 dword 数)、`prdtl`(PRDT 条数)、`write`(方向)、`ctba/ctbau`(命令表物理基址)。

PRDT 条目(`HBAPrdtEntry`):`dba/dbau`(缓冲物理地址)、`dbc`(数据字节数 **−1**,22 位)、`i`(完成中断)。

Register H2D FIS(`RegH2DFIS`,20 字节,放在命令表 `cfis[]`):

| 字段 | 值 |
|---|---|
| fis_type | `0x27`(REG_H2D) |
| flags | `0x80`(bit6=command) |
| command | 读 `0x25`(READ DMA EXT)/ 写 `0x35`(WRITE DMA EXT) |
| lba0-2 | LBA 低 24 位;lba3-5 = 高 24 位(48 位 LBA) |
| device | `0x40`(LBA 模式位) |
| count0/count1 | 扇区数低/高字节(16 位计数) |

流程:填 CFIS → 填 PRDT(指向物理连续缓冲,`dbc=count*512-1`)→ 设命令头(cfl/prdtl/write/ctba)→ `port.is = ~0U` 清中断 → `port.ci = 1<<slot` → 轮询 `ci` 清零 → 查 `tfd & 0x01`(ERR)。

## 约束与边界(本 tag 的真实限制)

- `read`/`write` 的 `buf` 是**物理地址**,缓冲必须物理连续且页对齐(一整页天然满足)。
- 只用 **slot 0、单命令串行**;`execute_command` 的 `slot` 参数实际固定指向 slot 0 的命令表。
- 用**非排队 DMA**(READ/WRITE DMA EXT),非 NCQ;扇区计数 16 位。
- 不处理热插拔、不全做错误恢复(仅查 `tfd.ERR`)、无块缓存、无分区/文件系统解析。
- BAR5 映射 flag 必须含 `FLAG_PCD`(MMIO 不可缓存);命令列表/FIS 页用 `phys + KERNEL_VMA` 直访,依赖高半区恒等映射。
- 仅在 QEMU 的 `ahci` + `ide-hd` 后端验证过,未跑真机。

## 验证入口

- host 逻辑镜像测:`ctest --test-dir build -R ahci --output-on-failure`。
- QEMU 机内集成测:`cmake --build build --target run-kernel-test`(会挂 `-device ahci` + `ahci_test.img`)。
- 测试盘生成:`scripts/create_ahci_test_disk.sh`(1MB,偏移 510-511 = `0x55 0xAA`)。
- 验收日志:`[AHCI] Read sector 0: 55 AA`。

## 源码索引

- PCI:[pci.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/pci/pci.hpp) / [pci.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/pci/pci.cpp) / [pci_config.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/pci/pci_config.hpp)
- AHCI:[ahci.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/ahci/ahci.hpp) / [ahci.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/ahci/ahci.cpp) / [ahci_config.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/ahci/ahci_config.hpp)
- 集成:[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp) Step 20-22;QEMU 配置 [qemu.cmake](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/cmake/qemu.cmake)
- 测试:[test_ahci.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_ahci.cpp) / [test_ahci.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_ahci.cpp)

## 权威依据

- Intel AHCI Specification rev 1.3:寄存器布局、端口状态机、Command List/FIS 尺寸、FIS 字段、PRDT 约定。
- OSDev — [PCI](https://wiki.osdev.org/PCI)、[AHCI](https://wiki.osdev.org/AHCI)。
