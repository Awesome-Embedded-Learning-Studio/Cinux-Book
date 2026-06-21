---
title: 008 · 为大内核铺路
---

# 008 · 为大内核铺路:ATA 驱动与 ELF 加载器

> mini kernel 从 [004](../01-boot/004-boot-load-mini-kernel.md) 诞生起,它的终极使命就一件事:把那个功能完整的 **big kernel** 从磁盘弄进内存、交权给它。前面几章我们给它配齐了输出、内存、异常——现在,是时候造"读盘"和"解析内核镜像"这两件家伙了。这一章我们写出 ATA PIO 磁盘驱动和 ELF64 加载器,把它们串成一条加载流水线。不过有个诚实的边界:big kernel 本身要到 [009](../03-big-kernel/009-large-kernel-entry.md) 才登场,所以这一章我们用两个 demo 验证这套流水线**能干活**,真正的加载+跳转留给下一章。

## 这一章我们要点亮什么

我们要造两样东西,再合成第三样。

一样是 **ATA PIO 磁盘驱动**:让 mini kernel 能直接对硬盘控制器下"读这几个扇区"的命令,把原始字节读进内存。PIO(Programmed I/O)意味着 CPU 亲自一个字一个字地从数据端口搬数据,不用 DMA、不用中断——最简单直接的法子。

另一样是 **ELF64 加载器**:内核镜像是标准的 ELF 可执行文件,带头部和若干"可加载段"(PT_LOAD)。加载器要读懂这个头部,把每个段从镜像里的位置搬到它该去的物理地址,多出来的 BSS 部分清零。

第三样是把它们串起来的 **big_kernel_loader**:先让 ATA 把整份内核镜像读到一个中转缓冲区(staging),再让 ELF 加载器解析它、把段各就各位,最后吐出入口地址。调用者拿到入口地址,理论上 `jmp` 过去就完成了接力。

但这一章 main 不会真去 `jmp`——因为 big kernel 还不存在。main 只用两个 demo 验证这套家伙是好的:读主引导扇区(MBR)看它的 `0xAA55` 签名对不对、读 mini kernel 所在的 LBA 16 试试 ELF 头解析(预期失败,因为 mini kernel 是裸二进制不是 ELF)。看到 MBR 签名 `VALID`、ELF 解析能正确判出"这不是 ELF",就说明读盘和解析这两条路都通了。

## 为什么现在需要它

mini kernel 自己其实不需要读盘——它已经被 bootloader 读进内存了。它会读盘,完全是为了伺候 big kernel。整个 004–008 这一段,mini kernel 的角色就是个"二传手":bootloader 把它弄起来,它把自己收拾利索(输出、内存、异常),然后要把 big kernel 也弄起来、交权。这最后一步"弄起来",就要求它能读磁盘、能解析 ELF。

为什么现在写、而不是等到 009 big kernel 出现了再写?因为这套加载流水线本身是个独立、可测的子系统。ATA 驱动的命令编码、ELF 头的字段解析,全是和"具体哪个内核"无关的纯逻辑,正好用 [005](005-mini-kernel-entry.md) 搭好的双轨测试狠狠磨一遍(host 单测验编码、QEMU 验真读盘)。等 009 big kernel 真的到来,这套已经验证过的流水线直接 `load_big_kernel()` 一调用就行,不用在"内核本身还没稳"的时候同时调试加载器。先把工具打牢,再上战场。

> 外部依据:OSDev 的 ATA PIO Mode 页详细描述了 ATA 控制器的 I/O 端口、状态位(BSY/DRQ/RDY/ERR)、LBA28/LBA48 寻址与 PIO 读扇区的时序(含 400ns 延时约定);ELF 页与 TIS ELF 规范定义了 Elf64_Ehdr/Elf64_Phdr 字段和 PT_LOAD 段语义。

## 设计图

先看磁盘长什么样。到现在,盘上已经住了三房客:MBR、stage2、mini kernel;big kernel 预订在更后面:

```text
扇区 0        MBR(512B,末尾 0xAA55)
扇区 1..15    Stage2
扇区 16..847  mini kernel(832 扇区 ≈ 416KB)
扇区 848+     big kernel(预订,009 才入住)
```

`big_kernel_loader` 的常量就是照这个布局定的:`BIG_KERNEL_LBA = 848`、`BIG_KERNEL_MAX_SECTORS = 512`(256KB 上界,够装任何合理的内核)。

再看 ATA 读一个扇区的 PIO 时序,精髓是"下命令 → 轮询状态 → 搬数据":

```text
read(lba, count, buf):
  选 master + LBA 模式,把 lba 拆字节写进 LBA_LOW/MID/HIGH 寄存器
  写 SECTOR_CNT = count
  写 COMMAND = READ_PIO(0x20)         ← 下命令
  for 每个扇区:
    delay 400ns                        ← ATA 规范要求,读控制口 4 次
    wait_data_ready()                  ← 轮询:BSY 清零 且 DRQ 置位
    for 256 次: buf[word] = inw(DATA)  ← 从 16 位数据端口搬 512 字节
```

最后是 ELF 加载这条流水线:

```text
big_kernel → staging@0x1000000(16MB,ATA 读来的原始 ELF)
   └─ load_elf:
        验 ELF magic(7F 45 4C 46)
        遍历 program header:
          PT_LOAD 段 → 从 p_offset 拷 p_filesz 字节到 p_paddr
                       再把 p_memsz - p_filesz 清零(BSS)
        返回 e_entry(入口地址)
```

## 代码路线

### 1. ATA PIO:轮询式读盘

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

### 2. ELF64:解析头部、定位 PT_LOAD 段

[elf_loader.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/elf_loader.hpp) 把 ELF64 的标准结构搬进来。文件头 `Elf64_Ehdr`(64 字节)里有 magic、类型(`ET_EXEC`)、架构(`EM_X86_64`)、入口地址 `e_entry`、程序头表偏移 `e_phoff`;程序头 `Elf64_Phdr`(56 字节)描述一个段:类型 `p_type`、文件偏移 `p_offset`、目标地址 `p_paddr`、文件大小 `p_filesz`、内存大小 `p_memsz`。

`parse_elf_header` 做最基本的校验:开头四个字节是不是 `0x7F 'E' 'L' 'F'`(magic)、是不是 64 位(`ELF_CLASS_64`)、小端、目标架构 x86-64、类型是不是可执行。这几项任一不对就返回 false。这个函数正是 008 两个 demo 之一要调的——拿 mini kernel 的 LBA 16 头几个字节去验,预期失败(它是裸二进制不是 ELF),用来证明"解析器能正确认出非 ELF"。

### 3. load_elf:拷 filesz、零填 BSS、返回 entry

真正的加载在 `load_elf` 里。它遍历程序头表,对每个 `PT_LOAD` 段做两件事:

```text
从 (镜像起点 + p_offset) 拷 p_filesz 字节 → p_paddr      // 段的实际内容
再从 (p_paddr + p_filesz) 起清零 (p_memsz - p_filesz) 字节  // BSS
```

为什么要区分 `p_filesz` 和 `p_memsz`?因为 ELF 里一个段在文件中只存"有初值"的部分(`p_filesz`),但它在内存里要占 `p_memsz` 那么大——多出来的就是 BSS(未初始化全局变量),文件里不存、加载时由加载器清零。`load_elf` 最后返回 `e_entry`,也就是内核的入口地址——调用者拿到它,理论上就能跳过去。

这里有个安全细节:`load_elf` 带 `staging_size` 参数,用来校验"段数据 (`p_offset + p_filesz`) 没超出我们实际从磁盘读进来的字节数"。因为我们读盘是按 `BIG_KERNEL_MAX_SECTORS`(256KB 上界)读的,真实内核可能更小,不做这个边界检查就可能从缓冲区外读到垃圾。

### 4. big_kernel_loader:把 ATA + ELF 串成一条流水线

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

### 5. main 的两个 demo:诚实说明 big kernel 未到

[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/main.cpp) 在 GDT/IDT/PMM/`int $3` 这些(沿用 006/007)之后,做这两步演示。第一步读 MBR:

```cpp
ata::read(0, 1, g_sector_buf);
uint16_t sig = g_sector_buf[510] | (g_sector_buf[511] << 8);
kprintf("[DEMO] MBR boot signature: 0x%04x %s\n", sig, sig == 0xAA55 ? "(VALID)" : "(INVALID)");
```

读第 0 扇区、看末两字节是不是 `0xAA55`——这正是 [001](../01-boot/001-boot-real-mode.md) MBR 立的签名。看到 `(VALID)`,说明 ATA 读盘这条路从头到尾通了(命令、轮询、`inw`、字节序都对)。

第二步读 mini kernel 所在的 LBA 16、试解析 ELF 头:

```cpp
ata::read(16, 1, g_sector_buf);
if (elf_loader::parse_elf_header(g_sector_buf)) { /* 是 ELF */ }
else { kprintf("No valid ELF header at LBA 16 (expected for flat binary)\n"); }
```

mini kernel 是 [004](../01-boot/004-boot-load-mini-kernel.md) 里 `objcopy -O binary` 出来的**裸二进制**(flat binary),没有 ELF 头,所以 `parse_elf_header` 返回 false 是**预期的**。这条 demo 的意义不是"找到 ELF",而是"证明解析器能正确地拒绝一个非 ELF",以及"再验一次读盘读到的是真实数据"。

main 最后打印 `Milestone 008 complete. Waiting for big kernel (009+)...` 然后 `cli; hlt`——一句话把这个 tag 的边界说清楚:家伙都造好了、也验证过了,就等 big kernel 入住。

## 调试现场

这一章没留 notes,但 ATA 和 ELF 各有几个典型坑。

ATA 头号坑是"悬空总线"。如果 `init` 后读状态寄存器得到 `0xFF`,说明这个通道根本没接硬盘(空插槽的总线被上拉到全 1)。继续往下发命令只会一直超时。Cinux 在 `init` 里专门判这个并报错。在 QEMU 里正常不会遇到,但移植到真机或换端口时会。

第二个是忘 400ns 延时或读错端口。下命令后立刻读状态寄存器,可能读到旧状态,`DRQ` 还没置位就以为"没数据"直接报错。修复就是那个读 4 次控制口的 `delay_400n`。判断:如果偶发性地"有时能读、有时超时",八成是时序。

第三个是数据宽度。误用 `inb`(8 位)读数据端口,一个扇区只读到一半、且奇偶字节错位。ATA 数据端口是 16 位的,必须 `inw` 读 256 次。

ELF 这边,坑在 magic 和段越界。magic 判错(比如只看前 3 字节漏了 `0x7F`)会放过非 ELF;段加载没做 `p_offset+p_filesz <= staging_size` 边界检查,就会从 staging 缓冲区外读到垃圾当段内容拷进去。`load_elf` 那个 `staging_size` 参数就是堵这个的。

## 验证

这一章的测试是整个 mini-kernel 卷里最重的,因为 ATA 命令编码、ELF 头解析全是纯逻辑,特别适合 host 单测。

host 单测(主力):

```bash
cmake --build build --target test_host
```

`test_ata`(五百多行)测命令编码和状态解析——给定 LBA/扇区数,验写进各寄存器的字节对不对、LBA28/48 的选择对不对;`test_elf_loader`(七百多行)测头解析和段加载——构造各种 ELF 头(合法的、magic 错的、class 错的)、各种 PT_LOAD 段(带 BSS 的、越界的),验加载行为;`test_big_kernel_loader` 测编排。这些都在 host 上跑,快且能在 CI 里反复磨。

QEMU 内核测试:

```bash
cmake --build build --target run-kernel-test
```

`test_ata`、`test_elf_loader` 在真内核里跑,验它们在真硬件(QEMU 的 ATA 控制器)上行为正确。

量产看 demo:

```bash
cmake --build build --target run
```

串口依次出现 `[INIT] ATA controller initialized`、`[DEMO] Reading MBR (LBA 0)...`、`MBR boot signature: 0xaa55 (VALID)`、`Reading mini kernel header (LBA 16)...`、`No valid ELF header at LBA 16 (expected for flat binary)`、最后 `Milestone 008 complete. Waiting for big kernel (009+)...`。`0xAA55 (VALID)` 和"expected for flat binary"这两行,就是这套加载流水线能干的铁证。

## 下一站

mini kernel 现在什么都备齐了:会输出、有内存、能扛异常、还能读盘解析 ELF。唯一缺的,是那个要被加载的主角——big kernel。这一章我们造好了枪、校好了准星,但靶子还没竖起来。

下一章 [009 · 大内核入口](../03-big-kernel/009-large-kernel-entry.md),big kernel 终于登场。它会被写进磁盘 LBA 848 之后,mini kernel 用这一章造好的 `load_big_kernel()` 把它读进来、加载好、跳进它的入口。那一刻,mini kernel 完成它全部的接力使命,把舞台正式交给功能完整的 big kernel——从那以后,Cinux 的故事就是 big kernel 的故事了。

---

### 参考

- OSDev — [ATA PIO Mode](https://wiki.osdev.org/ATA_PIO_Mode)(I/O 端口、状态位 BSY/DRQ/RDY/ERR、LBA28/LBA48、400ns 延时、READ SECTORS 命令)、[ELF](https://wiki.osdev.org/ELF)(Elf64_Ehdr/Elf64_Phdr、PT_LOAD、p_filesz/p_memsz 与 BSS)。
- Tool Interface Standard (TIS) ELF 规范 — ELF64 文件头与程序头字段定义、e_entry、PT_LOAD 段语义。
- 本 tag 源码:[ata.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/driver/ata.cpp)/[ata.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/driver/ata.hpp)、[elf_loader.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/elf_loader.hpp)/[elf_loader.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/elf_loader.cpp)、[big_kernel_loader.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/big_kernel_loader.cpp)/[big_kernel_loader.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/big_kernel_loader.hpp)、[string.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/lib/string.cpp)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/main.cpp)、[test_ata.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_ata.cpp)/[test_elf_loader.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_elf_loader.cpp)/[test_big_kernel_loader.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_big_kernel_loader.cpp)。

> 参考 URL 的有效性会在全局审查阶段用 open-websearch(bing)统一核活。
