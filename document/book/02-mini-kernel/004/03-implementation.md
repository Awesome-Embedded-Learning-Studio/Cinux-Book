---
title: 03 · 代码路线:ATA PIO、ELF 解析、load_elf、loader、demo
---

# 代码路线:ATA PIO、ELF 解析、load_elf、loader、demo

## 1. ATA PIO:轮询式读盘

[ata.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/driver/ata.cpp) 直接对 ATA 控制器的 I/O 端口下命令。主通道基址 `0x1F0`,控制口 `0x3F6`,各寄存器按偏移区分(`ata.hpp` 里全列了):

```cpp
constexpr uint16_t ATA_PRIMARY_BASE = 0x1F0;
// 偏移:DATA=0, SECTOR_CNT=2, LBA_LOW=3, LBA_MID=4, LBA_HIGH=5, DRIVE=6, STATUS=7
constexpr uint8_t ATA_STATUS_BSY = 0x80;   // 忙
constexpr uint8_t ATA_STATUS_DRQ = 0x08;   // 数据就绪
constexpr uint8_t ATA_STATUS_RDY = 0x40;   // 驱动器就绪
constexpr uint8_t ATA_CMD_READ_PIO     = 0x20;  // LBA28 读
constexpr uint8_t ATA_CMD_READ_PIO_EXT = 0x24;  // LBA48 读
```

`init` 先做软件复位(控制口写 SRST 位)、选 master 盘、轮询等 `RDY` 置位,还要判一个坑:如果状态读出来是 `0xFF`,说明总线上根本没接硬盘(悬空总线拉到全 1),这时别再往下走。

`read` 是核心。它先按 LBA 大小自适应寻址——LBA 小于 28 位用 LBA28(把 lba 拆 4 字节、命令 `0x20`),否则用 LBA48(高低位各写一遍、命令 `0x24`)。下完命令,逐扇区轮询:`delay_400n` 之后 `wait_data_ready` 等 `BSY` 清零且 `DRQ` 置位,然后从 16 位数据端口连续 `inw` 256 次,正好搬走一个扇区 512 字节。

这里有两个 ATA 特有的讲究。一是 **400ns 延时**:ATA 规范要求下命令后、轮询状态前等 400 纳秒,否则可能读到命令还没生效时的旧状态。Cinux 的做法是读 4 次控制口(每次约 100ns),既满足延时、又不碰状态寄存器(读状态寄存器会清掉某些中断位)。二是 `inw` 读的是 **16 位**:ATA 数据端口一次吐 2 字节,所以一个 512 字节扇区是 256 次 `inw`,不是 512 次 `inb`——这点写错,数据要么读一半、要么错位。

## 2. ELF64:解析头部、定位 PT_LOAD 段

[elf_loader.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/elf_loader.hpp) 把 ELF64 的标准结构搬进来。文件头 `Elf64_Ehdr`(64 字节)里有 magic、类型(`ET_EXEC`)、架构(`EM_X86_64`)、入口地址 `e_entry`、程序头表偏移 `e_phoff`;程序头 `Elf64_Phdr`(56 字节)描述一个段:类型 `p_type`、文件偏移 `p_offset`、目标地址 `p_paddr`、文件大小 `p_filesz`、内存大小 `p_memsz`。

`parse_elf_header` 做最基本的校验:开头四个字节是不是 `0x7F 'E' 'L' 'F'`(magic)、是不是 64 位(`ELF_CLASS_64`)、小端、目标架构 x86-64、类型是不是可执行。这几项任一不对就返回 false。这个函数正是 008 两个 demo 之一要调的——拿 mini kernel 的 LBA 16 头几个字节去验,预期失败(它是裸二进制不是 ELF),用来证明"解析器能正确认出非 ELF"。

## 3. load_elf:拷 filesz、零填 BSS、返回 entry

真正的加载在 `load_elf` 里。它遍历程序头表,对每个 `PT_LOAD` 段做两件事:

```text
从 (镜像起点 + p_offset) 拷 p_filesz 字节 → p_paddr      // 段的实际内容
再从 (p_paddr + p_filesz) 起清零 (p_memsz - p_filesz) 字节  // BSS
```

为什么要区分 `p_filesz` 和 `p_memsz`?因为 ELF 里一个段在文件中只存"有初值"的部分(`p_filesz`),但它在内存里要占 `p_memsz` 那么大——多出来的就是 BSS(未初始化全局变量),文件里不存、加载时由加载器清零。`load_elf` 最后返回 `e_entry`,也就是内核的入口地址——调用者拿到它,理论上就能跳过去。

这里有个安全细节:`load_elf` 带 `staging_size` 参数,用来校验"段数据 (`p_offset + p_filesz`) 没超出我们实际从磁盘读进来的字节数"。因为我们读盘是按 `BIG_KERNEL_MAX_SECTORS`(256KB 上界)读的,真实内核可能更小,不做这个边界检查就可能从缓冲区外读到垃圾。

## 4. big_kernel_loader:把 ATA + ELF 串成一条流水线

[big_kernel_loader.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/big_kernel_loader.cpp) 就是把上面两节拼起来,逻辑非常直:

```cpp
uint64_t load_big_kernel(uint64_t disk_lba) {
    ata::read(disk_lba, BIG_KERNEL_MAX_SECTORS, (void*)BIG_KERNEL_LOAD_ADDR); // 读到 staging@16MB
    // 验 staging 开头是不是 ELF magic
    if (magic 不是 7F 45 4C 46) return 0;
    return elf_loader::load_elf((void*)BIG_KERNEL_LOAD_ADDR, staging_bytes);   // 解析+加载,返回入口
}
```

`BIG_KERNEL_LOAD_ADDR = 0x1000000`(16MB)是 staging 缓冲区——选这么高,是为了避开 mini kernel(在 `0x20000`)、bootloader 结构(<0x10000)和 PMM 管的可分配区。读盘、验 magic、加载,三步一气呵成,返回 big kernel 的入口地址。

再说一遍那个重要的边界:这个函数**写好了、但 008 的 main 没有调用它**。因为现在盘上 LBA 848 之后还没有真正的 big kernel,调了也是读到一堆零或垃圾、magic 校验失败。它要等 009 big kernel 真正被编出来、写进磁盘,才会被真正调用、真正完成接力。

## 5. main 的两个 demo:诚实说明 big kernel 未到

[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/main.cpp) 在 GDT/IDT/PMM/`int $3` 这些(沿用 006/007)之后,做这两步演示。第一步读 MBR:

```cpp
ata::read(0, 1, g_sector_buf);
uint16_t sig = g_sector_buf[510] | (g_sector_buf[511] << 8);
kprintf("[DEMO] MBR boot signature: 0x%04x %s\n", sig, sig == 0xAA55 ? "(VALID)" : "(INVALID)");
```

读第 0 扇区、看末两字节是不是 `0xAA55`——这正是 [001](../01-boot/001/) MBR 立的签名。看到 `(VALID)`,说明 ATA 读盘这条路从头到尾通了(命令、轮询、`inw`、字节序都对)。

第二步读 mini kernel 所在的 LBA 16、试解析 ELF 头:

```cpp
ata::read(16, 1, g_sector_buf);
if (elf_loader::parse_elf_header(g_sector_buf)) { /* 是 ELF */ }
else { kprintf("No valid ELF header at LBA 16 (expected for flat binary)\n"); }
```

mini kernel 是 [004](../01-boot/004/) 里 `objcopy -O binary` 出来的**裸二进制**(flat binary),没有 ELF 头,所以 `parse_elf_header` 返回 false 是**预期的**。这条 demo 的意义不是"找到 ELF",而是"证明解析器能正确地拒绝一个非 ELF",以及"再验一次读盘读到的是真实数据"。

main 最后打印 `Milestone 008 complete. Waiting for big kernel (009+)...` 然后 `cli; hlt`——一句话把这个 tag 的边界说清楚:家伙都造好了、也验证过了,就等 big kernel 入住。
