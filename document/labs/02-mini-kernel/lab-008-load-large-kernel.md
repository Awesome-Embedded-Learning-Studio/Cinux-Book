---
title: Lab 008 · 为大内核铺路
---

# Lab 008 · 为大内核铺路

> 这个 lab 配套 [008 · 为大内核铺路](../../book/02-mini-kernel/008-load-large-kernel.md)。目标:写出 ATA PIO 读盘驱动和 ELF64 加载器,串成 `load_big_kernel`,并用两个 demo(读 MBR 验签名、试解析 LBA 16)验证它们能干活。**big kernel 还没来(009),这一章不真正加载/跳转**,只搭管线 + 演示。

## 实验目标

- 写 ATA PIO 驱动:`init`(复位、选 master、查 RDY、判 0xFF 悬空)、`read(lba,count,buf)`(LBA28/48 自适应、轮询 DRQ、`inw`×256、400ns delay)。
- 写 ELF64 解析器:`Elf64_Ehdr`/`Elf64_Phdr` 结构、`parse_elf_header` 验 magic/class/machine/type、`load_elf`(拷 filesz、零填 BSS、返回 entry,带 staging 越界检查)。
- 写 `big_kernel_loader::load_big_kernel`:ATA 读 staging@16MB → 验 ELF magic → `load_elf`。
- main 加两个 demo:读 MBR(LBA 0)验 `0xAA55`、读 LBA 16 试 `parse_elf_header`(预期 flat binary 失败)。
- 配 host 重测(ATA 命令编码、ELF 头/段加载)。

## 前置条件

- 完成 [Lab 007](lab-007-mini-kernel-intr.md):内核有 kprintf、GDT/IDT/异常、QEMU 测试框架。
- 理解 I/O 端口、ATA PIO 时序、ELF 文件格式(头、PT_LOAD、filesz/memsz)。

## 任务分解

分五块走。

第一块,ATA 驱动基础。定义端口/寄存器/状态位/命令常量(`0x1F0`/`0x3F6`、BSY/DRQ/RDY/ERR、READ_PIO `0x20`/EXT `0x24`、`ATA_DRIVE_MASTER=0xE0`)。写 `init`:控制口写 SRST 复位、`delay_400n`、选 master、轮询 `wait_not_busy`、判状态不是 `0xFF`、`RDY` 置位。

第二块,ATA 读。`read(lba,count,buf)`:参数校验 → `wait_not_busy` → 按 LBA 大小选 LBA28(拆 4 字节、`0x20`)或 LBA48(高低各写一遍、`0x24`)→ 逐扇区:`delay_400n`、`wait_data_ready`(BSY 清且 DRQ 置,顺带查 ERR/DF)、`inw` ×256 读 512B。写 `delay_400n`(读控制口 4 次)和 `wait_not_busy`/`wait_data_ready` 轮询。

第三块,ELF 解析。定义 `Elf64_Ehdr`(64B)、`Elf64_Phdr`(56B)、常量(`ELF_MAGIC=0x464C457F`、`ELF_CLASS_64`、`ET_EXEC`、`EM_X86_64=62`、`PT_LOAD=1`)。`parse_elf_header` 验 magic/class/data/machine/type。`load_elf(src, staging_size)`:遍历 program header,PT_LOAD 段从 `p_offset` 拷 `p_filesz` 到 `p_paddr`、清零 `p_memsz-p_filesz`(BSS),每段检查 `p_offset+p_filesz` 不超 `staging_size`,返回 `e_entry`。

第四块,编排。`big_kernel_loader::load_big_kernel(lba)`:常量 `BIG_KERNEL_LOAD_ADDR=0x1000000`、`BIG_KERNEL_LBA=848`、`BIG_KERNEL_MAX_SECTORS=512`。ATA 读 staging → 验开头 `7F 45 4C 46` → `load_elf` → 返回 entry。

第五块,main demo + 测试。main 里 `ata::init` 后:读 LBA 0 验 `g_sector_buf[510..511]==0xAA55`;读 LBA 16 调 `parse_elf_header`(预期 false)。配 `test/unit/test_ata.cpp`、`test_elf_loader.cpp`、`test_big_kernel_loader.cpp`(host 验编码/解析/加载),`test/test_ata.cpp`、`test_elf_loader.cpp`(QEMU)。

## 接口约束

这些得自己保证对、lab 不给现成代码:

- ATA 端口 `0x1F0`/控制口 `0x3F6`;数据端口 16 位(`inw`,一个扇区 256 次,不是 512 次 `inb`)。
- 状态位:ERR 0x01、DRQ 0x08、DF 0x20、RDY 0x40、BSY 0x80;`wait_data_ready` 要 BSY 清且 DRQ 置。
- 命令 LBA28 `0x20`/LBA48 `0x24`;选盘 `0xE0`(master + LBA 位);下命令后必须 400ns delay。
- ELF:magic `0x7F 'E' 'L' 'F'`(`0x464C457F` 小端)、`PT_LOAD=1`;加载按 `p_paddr` 落位、拷 `p_filesz`、清零 `p_memsz-p_filesz`、返回 `e_entry`;做 staging 越界检查。
- staging `0x1000000`(16MB)、big kernel LBA 848、最多 512 扇区;MBR 签名在字节 `[510..511]` = `0xAA55`(小端 `55 AA`)。
- **边界:本 lab 不调用 `load_big_kernel`、不跳转**——只 demo 读 MBR + 解析头。

## 验证步骤

host 单测(主力):

```bash
cmake --build build --target test_host
```

`test_ata`/`test_elf_loader`/`test_big_kernel_loader` 验编码与加载逻辑。

QEMU 内核测试:

```bash
cmake --build build --target run-kernel-test
```

量产看 demo:

```bash
cmake --build build --target run
```

串口出现 `ATA controller initialized`、`MBR boot signature: 0xaa55 (VALID)`、`No valid ELF header at LBA 16 (expected for flat binary)`、最后 `Milestone 008 complete. Waiting for big kernel (009+)...`。

## 常见故障

`init` 后状态读出 `0xFF`、一直超时。悬空总线(该通道没接盘)。判 `0xFF` 报错;确认 QEMU 启动带了硬盘、端口对。

偶发"有时能读有时超时"。少了 400ns delay,或读了状态寄存器(会清中断位)而非控制口。补 `delay_400n`,且 delay 读控制口不读状态。

数据只读到一半/错位。误用 `inb` 读 16 位数据端口。改 `inw` ×256。

ELF magic 漏判(放过非 ELF)或段加载越界。magic 只查了 3 字节漏 `0x7F`;或没做 `p_offset+p_filesz <= staging_size`。补全 magic 四字节、加越界检查。

main 跑着跑着跳进了垃圾地址。误以为本 tag 要调用 `load_big_kernel` 并跳转——big kernel 不存在,读到的 staging 是空的或垃圾,magic 校验就该拦下,根本到不了跳转。本 lab 不调用它。

## 通过标准

- `test_host` 里 ATA/ELF/loader 三套 host 单测全过。
- `run-kernel-test` 里 `test_ata`/`test_elf_loader` 过、退出码 0。
- 量产 `make run` 看到 MBR `0xAA55 (VALID)`、LBA 16 `expected for flat binary`、`Milestone 008 complete. Waiting for big kernel (009+)`。
- 全程没调用 `load_big_kernel`、没跳转——那是 [009](../../book/03-big-kernel/009-large-kernel-entry.md) 的事。
