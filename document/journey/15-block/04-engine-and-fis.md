---
title: "04 · 引擎起停,和对讲的黑话"
description: "停引擎的次序:清旧错、停命令引擎等 CR 自清、停接收引擎等 FR 自清,各有 50 拍预算。三页内存安家:命令页、FIS 页、数据页。命令头 32 字节、命令表 144 字节,座位表 static_assert 锁死。FIS 黑话:0x27 开头,命令位在 bit7,48 位地址围着 device 寄存器插空躺。"
chapter: 15
order: 4
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - storage
  - ahci
---

# 引擎起停,和对讲的黑话

寄存器窗开了,口也挑定了,可口的引擎还转着别人的旧差事——固件留下的状态,咱们一件也信不得。接手的标准姿势是停干净再重启,次序规范写得明白,咱们照着走:

```cpp
port.sata_error = ~0U;
port.command    = (AhciReg{port.command} & ~kCmdStart).raw;
wait_clear_observed(&port.command, kCmdCommandListRunning, kStopBudgetTicks,
                    "command engine stop");
port.command = (AhciReg{port.command} & ~kCmdFisReceiveEnable).raw;
wait_clear_observed(&port.command, kCmdFisReceiveRunning, kStopBudgetTicks,
                    "receive engine stop");
```

头一行,往错误寄存器写全 1——它的错误位是写 1 清零的脾气,固件攒下的旧错,咱们一口气抹平。然后摘掉 ST 位,请命令引擎停下,等 CR 位自己落下来。再摘 FRE,请接收引擎停,等 FR 落下。两个等待各给 50 拍预算——时钟 100 Hz,合五百毫秒,跟注释里记的那句规范时限(请软件允许五百毫秒)对得上。引擎停稳,咱们给它安三个新家:

```cpp
command_page_ = mm::Pmm::self().allocate_page();
fis_page_     = mm::Pmm::self().allocate_page();
data_page_    = mm::Pmm::self().allocate_page();
cinux::base::SetBytes(cinux::base::PtrAt<void>(mm::DirectMapVirt(command_page_.raw)), 0,
                      cinux::arch::page::kSize);
port.command_list_base       = static_cast<uint32_t>(command_page_.raw);
port.command_list_base_upper = 0;
port.fis_base                = static_cast<uint32_t>(fis_page_.raw);
port.fis_base_upper          = 0;
```

命令页是给控制器看的命令清单,一个页里住着 32 个命令头,页尾还有一张命令表。FIS 页是设备发来的信件投递处。数据页是咱们自己的中转站台,盘跟内存倒字节,走的都是它。基址寄存器是 32 位一栏,高半栏留给 4 GiB 以上的机器,咱们写 0。三个家安顿好,重启的次序跟停机正好倒着走:开 FRE,等 FR 亮。验一遍 TFD 确实闲着。开 ST,等 CR 亮。到这里,引擎空转起来了,只是还没有一道命令进去过。

命令怎么写进去?这是本卷对讲黑话最密的地方,咱们慢慢认。控制器的命令清单,每格是一个 32 字节的命令头,里头三样要紧东西:本格的 FIS 有多长、带几条数据腿、方向是读是写。还有一个指针,指去命令表里真正的命令本体。座位表全拿 static_assert 锁着:

```cpp
static_assert(sizeof(CommandHeader) == 32, "command headers are 32 bytes");
static_assert(sizeof(CommandTable) == 0x90, "the command table is 144 bytes");
static_assert(kCommandTableOffset == 32 * sizeof(CommandHeader),
              "the table follows all thirty-two command headers");
```

命令表紧跟着 32 个头,落在命令页的 0x400 偏移上——32 格头,每格 32 字节,乘出来正好 0x400。表里头一个住客是 FIS,全名 frame information structure,帧信息结构:咱们发给设备的每个命令,都填成一条 64 字节定长的信。寄存器型的信,头一个字节是 `0x27`,宣告这是主机写给设备的一封。第二个字节是旗子,命令要落在 bit 7。说到这个 bit,笔者得翻一桩当年的旧案:头一遍搓这块的时候,手边的笔记把命令位记在了 bit 6,信照填、命令照发,盘一声不吭——bit 6 单独置上,设备什么也不执行。对着规范的表格数了三遍,才发现旗子差了一格。这一课,笔者把它写成了代码头顶的一行 static_assert:

```cpp
static_assert(kFisFlagCommand == 0x80,
              "the command flag is bit seven, not the scouting note's six");
```

信的正文,是 ATA 的一套词汇。操作码一字节,咱们用得上的四个都起了名字:认盘用 `0xEC`(IDENTIFY),读扇区用 `0x25`(READ DMA EXT),写扇区用 `0x35`(WRITE DMA EXT),催盘把写缓存落实用 `0xEA`(FLUSH)。扇区地址 48 位,在信里躺法特别:低 3 字节、device 寄存器、高 3 字节,围着中间那个字节插空摆开——

```cpp
fis.lba_low[byte]  = static_cast<uint8_t>(kAddress.extract(kLowByte));
fis.lba_high[byte] = static_cast<uint8_t>(kAddress.extract(kHighByte));
```

宿主测试里有一案,咱们拿 `0x0001020304050607` 这么个一眼能数清的地址喂进去,验的就是这个插空的躺法。信写完,回头填命令头和命令表的数据腿:一条腿 16 字节,记着数据页的物理地址和字节数。字节数按规范减一落位,最高位是要完成中断的请帖。数据腿几条、方向如何,一起进命令头的旗子里。

发射的动作,咱们写得反而最短:

```cpp
completion_.rearm();
port.interrupt_status  = kWatched.raw;
asm volatile("" ::: "memory");
port.command_issue = kSlotZero.raw;
```

头一拍,判决书重置。状态寄存器把要盯的位清一遍(它也是写 1 清零),中间垫一道编译器栅栏,把清状态和发命令隔成前后两拍,末了往 `PxCI` 写上 0 号槽的位——控制器看见这根线,自己起身取信、搬数据。咱们这一卷的命令全走 0 号槽,一次一道,盘的脾气就是这样,一道没完,下一道不接。

引擎转起来了,黑话也认全了。下一节,咱们发出第一封信——认盘的那封。
