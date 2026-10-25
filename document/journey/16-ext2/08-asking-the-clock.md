---
title: "08 · 问芯片几点了"
description: "时间戳要 Unix 秒,钟问 CMOS 芯片要:两只端口一问一答,读两遍全同才采信,BCD、12 小时、世纪寄存器各有各的方言,解码是纯函数,宿主上四种组合都驱过。ext2 不认得芯片,构造时收一只函数指针,盖的是递进来的钟。"
chapter: 16
order: 8
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - storage
  - rtc
---

# 问芯片几点了

stat 屏上的 mtime,是 mkfs 烤盘那一刻盖下的戳。咱们自己往卷上写文件,戳也得盖——Unix 秒从哪儿来?主板上有块芯片,机器断了电它都在走:CMOS 的实时钟。

芯片的脾气,两只端口:0x70 报寄存器号,0x71 收数据——一问一答,PCI 配置空间那对端口的手艺。钟面上七个寄存器:秒、分、时、日、月、年,外加一个世纪寄存器。读它有座绕不开的坎:芯片一秒一跳,您读到秒、它翻到分,拼出来的就是四不像。解法老老实实,咱们读两遍,七个字节全同才采信,不同,再来:

```cpp
RawClock raw = read_raw_once();
for (RawClock again = read_raw_once(); !same_raw(raw, again); again = read_raw_once()) {
    raw = again;
}
```

读到手的字节还不能直接用,芯片说的是方言:BCD 还是纯二进制,12 小时还是 24 小时,寄存器 B 上两个格式位说了算。这些弯弯绕,咱们全装在一个纯函数里,不碰一只端口。BCD 的换算好懂,咱们一位一位地拆着换:

```cpp
const auto kFromBcd = [](uint8_t bcd) {
    const auto kLow  = static_cast<uint8_t>(bcd & base::bit::Ones<uint8_t>(4));
    const auto kHigh = static_cast<uint8_t>(bcd >> 4U);
    return static_cast<uint8_t>(kLow + (kHigh * 10U));
};
```

一个字节攥的是两位十进制:低四位放的是个位,高四位放的是十位,咱们取出来一乘一加,别的就没有了。12 小时制是最绕的,下午的钟点带一根 PM 位,午夜十二点又是个特例,咱们处理它用的几行长这样:

```cpp
const bool kAfternoon =
    base::bit::BitMask<uint8_t>{raw.hours}.has(base::bit::MaskBit<uint8_t>(7));
const auto    kHourBits = static_cast<uint8_t>(raw.hours & base::bit::Ones<uint8_t>(7));
unsigned long hours     = kBinary ? kHourBits : kFromBcd(kHourBits);
if (!kCivil) {
    if (kAfternoon && hours != 12UL) {
        hours += 12UL;
    } else if (!kAfternoon && hours == 12UL) {
        hours = 0UL;
    }
}
```

小时字节的第 7 位是 PM 旗,咱们得拿 Ones(7) 把旗摘出去,剩下的位才走换算。底下的两个分支各管一头:PM 旗亮着而钟点不是 12,加 12 就折成了 24 时制。PM 旗没亮而钟点是 12,那是午夜的零点,就归成零了。头一个分支里 hours != 12UL 那道挡门是承重的:下午 12 点折过来本来就是 24 时制的 12,再加一下就翻成了 24,凭空多出了一个钟头。咱们这个纯函数不碰端口,宿主的测试才能把 BCD 或二进制、12 或 24 时四种组合挨个驱过,机器上难得碰见的半夜十二点,宿主上就跑得出来了。

世纪寄存器还得防一手,有的板子压根没装它,缺了,咱们就按内核自己的发布年份(代码里写的是 2026)猜个世纪,猜到 2099 年都不会错。

七个字段,最后折成一个数:1970 年 1 月 1 日以来的秒。这套算术住在 base/time,咱们看看它的路数:月份挪到三月起头,闰日落在年尾,四百年 146097 天,一口气数完。挪月的手法是历法库的老手艺,days_from_civil,Howard Hinnant 的,咱们照手艺搬进一个编译期的常量函数。顺带安家的还有时长的强类型:500_ms 是 Milliseconds,会听那一卷 Hz 的老配方,两家人从此住一个头文件。

ext2 盖戳,自己却不认得芯片,咱们构造时递一只函数指针进去:

```cpp
using ClockFn = unsigned long long (*)();
explicit Ext2(driver::BlockCache& cache, ClockFn clock = nullptr);
```

盖谁递来的钟,咱们各递各的。机器上递的是实时钟,开机报一次面板,盖戳的时候现读。宿主的测试递回一只不走的钟,时间戳案案对得上。不缓存,不轮询,一秒一跳的芯片,咱们用不着替它操心。

家当到这儿齐了。可开机的故事还是老的:主流程一条道走到黑。这一卷,咱们换个讲法。
