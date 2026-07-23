---
title: 070 · HPET 与 RTC:给内核两种时间(monotonic 与墙钟)
---

# 070 · HPET 与 RTC:给内核两种时间(monotonic 与墙钟)

> 到现在内核只有一种时间源——PIT(可编程间隔定时器),用来产生调度滴答。可程序常常要两种不同的时间:`clock_gettime(CLOCK_MONOTONIC)` 要的是「自开机以来过了多久」(测间隔、算超时,不受墙钟拨动影响);`clock_gettime(CLOCK_REALTIME)` 要的是「现在几点几分」(真世界时间,Unix epoch)。这俩是不同的语义,得两个不同的硬件源。这一章立两个时钟驱动:**HPET**(高精度定时器,做一个 free-running 计数器,给 monotonic 纳秒)和 **RTC**(实时时钟,CMOS 里那个,给墙钟的 Unix epoch),然后接到 `sys_clock_gettime`——MONOTONIC 读 HPET、REALTIME 用 RTC 的开机时刻加上 HPET 走过的 delta。
>
> B 档:这一章交付的是内部机制(两种时钟 + clock_gettime 接线),没有用户可见的新能力。验证靠机制测(计数器真在走、不是冻死)+ host 纯算术单测(ticks→ns 换算、BCD 解码、日期→epoch)+ boot 日志打出真实时间。一条诚实的边界先说在前头:HPET 这里只做**计数器**(monotonic 用),没做它的定时中断功能(给 TCP 重传、调度抢占那种「到点叫我」的 timer);RTC 只在 boot 读一次(端口 I/O 慢),墙钟靠 RTC 基线 + HPET delta 推进,不周期重同步。

## 这章咱们要点亮什么

1. **两种时钟语义**:monotonic(boot-relative,不受墙钟调整,测间隔)vs 墙钟(Unix epoch,真世界时间)。它们要不同的硬件源,不能混。
2. **HPET 当 free-running 计数器**:ACPI 表找到它的 MMIO 基址,映射进来,置 ENABLE 让主计数器走起来,记下 boot 时的计数当基线;monotonic = 现在计数 − boot 基线,换算成纳秒。
3. **HPET 的两个实机坑**:General Config 寄存器在 `0x010`(不是 `0x008`,HPET 寄存器间距 0x10);QEMU 丢 64-bit MMIO 写,得一律 32-bit 访问。
4. **RTC 读墙钟**:经典 0x70/0x71 端口 I/O,默认 BCD 编码,转 binary 后用 Hinnant 的 `days_from_civil` 算 Unix epoch。读的时候要防「读一半日期更新了」的撕裂(UIP + 两读一致)。
5. **clock_gettime 怎么接**:MONOTONIC 读 HPET;REALTIME = RTC boot epoch(粗,秒级)+ HPET 走过的 delta(精,纳秒)。

## 两种时间,两个源

先分清两种时钟,这是这一章的概念地基。

**monotonic(单调时钟)**:从某个固定点(这里选开机)开始计,只往前走,不受墙钟调整影响。用途是「这件事跑了多久」「超时没」「两次读之间隔多少」——关心的是**差值**,绝对值无所谓。它要的硬件源是一个**持续递增的计数器**:HPET 的主计数器正合适(置 ENABLE 后 free-running,从不回拨)。

**墙钟(REALTIME)**:真世界时间,「现在是 2026 年几月几日几点几分」,Unix epoch 秒。用途是给用户看时间、打时间戳。它要的硬件源是一个**知道现在日期时间的设备**:RTC(CMOS 里那个,主板电池维持着)。

> 为什么不都用一个?因为语义冲突。monotonic 不能受墙钟拨动——你把系统时间往后调一小时,monotonic 测的「这段代码跑了 3 秒」不能变成「跑了 3603 秒」。墙钟又得是真世界时间(不能是「开机以来」)。所以 Linux 有 `CLOCK_MONOTONIC` 和 `CLOCK_REALTIME` 两套,各接各的源。这一章给内核也备齐这两套。

## HPET:free-running 计数器 → monotonic 纳秒

HPET(High Precision Event Timer)是个 MMIO 设备。第一步是找到它在哪——ACPI 表里有个 HPET 描述符,解出来基址(QEMU 下是物理 `0xFED00000`)。第二步把这块物理内存映进内核的 MMIO 窗(_uncached_,照 LAPIC/xHCI/e1000 的范式),就能读写它的寄存器了。

`init()` 干这么几件事(`hpet.cpp`):读周期(period,计数器走一格多少飞秒)、置 ENABLE 让主计数器走起来、把当前计数记成 `boot_counter_`(基线)。之后 `monotonic_ns()` 就是:

```cpp
inline uint64_t ticks_to_ns(uint64_t period_fs, uint64_t ticks) {
    // 溢出安全:拆成 hi/lo,中间积压在 uint64 内几百年不溢
    return (ticks / kDivisor) * period_fs + ((ticks % kDivisor) * period_fs) / kDivisor;
}
// monotonic_ns() = ticks_to_ns(period_fs_, read_main_counter() - boot_counter_);
```

(`hpet.hpp:54`。)`ticks_to_ns` 写成 inline 纯函数放头里,就是为了 host 能直接测——朴素乘法 `ticks * period` 在 100MHz 跑一年会溢出 uint64,得拆成 hi/lo 两段算。host 单测就罩这个换算(已知 period/ticks → 预期 ns,含 100MHz 跑一年不溢)。

这一步有两个实机教训出来的坑,值得记(都是真踩过的)。

**坑一:General Config 寄存器在 `0x010`,不是 `0x008`。** HPET 的通用寄存器**间距是 0x10**(不是密集的 8 字节):`0x000` 是 Capabilities、`0x010` 是 General Config(ENABLE 位在这)、`0x0F0` 是主计数器。一开始按「密集布局」把 Config 当 `0x008`,写 ENABLE 死活不生效(读回恒 0)——因为 `0x008` 落在 reserved 区,写进去全丢。改成 `0x010` 立刻通。还有个相关的:`period` 在 Capabilities 寄存器的**高 32 位**,不是低 32(低 32 是 vendor+计时器数);`read32(Caps + 4)` 直接拿高半。

**坑二:一律 32-bit 访问,QEMU 丢 64-bit 写。** QEMU(和部分真机)会丢 64-bit MMIO 写——64-bit 读碰巧能用(QEMU 内部拆两次 32-bit 读再拼),但 64-bit 写读回是 0。所以一律 32-bit:period 读高半、ENABLE 是 `write32(Config, read32(Config)|1)`、64-bit 主计数器分两半读(`read32(0x0F0)` + `read32(0x0F4)`)还要 rollover 保护——读高半、低半、再读高半,两次高半不一致说明低半读完后跨过了一个 32 位边界,重读低半(`hpet.cpp:52`,标准无 race 算法)。

> 这两个坑的诊断套路值得记:读对/写错 → 怀疑访问宽度(64-bit 写被丢);period 取错半 → 看 raw 64-bit 值定位字段;寄存器写不生效 → 核对偏移(QEMU 源码 `HPET_CFG` 宏是权威)。boot 日志会打 `[HPET] MMIO 0xFED00000, period 10000000 fs (100000000 Hz), counter enabled`——period 10000000 飞秒 = 100 MHz,跟 QEMU 一致。

## RTC:CMOS 端口 → Unix epoch

RTC 是另一个路子——它不在 MMIO,是经典的 0x70/0x71 端口 I/O(MC146818 兼容),且默认用 BCD 编码(0x26 表示 26)。读法(`rtc.cpp`):

```cpp
uint8_t read_reg(uint8_t index) {
    outb(0x70, 0x80 | index);   // bit7 关 NMI,防 read 中途 NMI 踩 RTC
    return inb(0x71);
}
```

`read_datetime()` 读年月日时分秒(+ 世纪寄存器 0x32),转 binary,然后算 Unix epoch。这里两个关键:

**算 epoch 不手搓,用 Hinnant 算法。** 公历「年月日 → 距 1970 的天数」手搓容易错(闰年规则、月份天数)。用 Howard Hinnant 的 `days_from_civil`(`rtc.hpp:58`)——它是个精确且溢出安全的 proleptic Gregorian 算法,写成 inline 纯函数,host 能拿已知日对照测(1970-01-01=0、2000-01-01=946684800、2024-01-01=1704067200、闰日)。`datetime_to_unix_seconds = days_from_civil(...)*86400 + h*3600 + m*60 + s`。

**防撕裂:UIP + 两读一致。** RTC 内部 1Hz 更新寄存器,更新到一半你去读,可能读到「秒已经进位但分还没」的撕裂值。防法:先读 Status A 的 UIP 位(更新进行中标志),等它清;然后连续读两次,两次一致才算数(4 轮封顶,测试内核全程关中断 + QEMU 更新慢,立刻收敛)。还有 12h/BCD 模式不能假设——读 Status B 判是 BCD 还是 binary、是 12h 还是 24h,自适应(12h 的 PM 标志在 raw hours 字节的 bit7,BCD 解码前 mask 掉、解码后加 12)。

boot 时 `init()` 读一次,缓存 `boot_epoch_seconds_` 当墙钟基线。boot 日志打真实时间:`[RTC] 2026-06-30 06:35:58 (epoch 1782801358)`——读出的日期跟宿主机时钟一致(BCD + 世纪 + epoch 全对)。

> 为什么 boot 只读一次?端口 I/O 慢,而且周期重同步要 RTC 的更新结束 IRQ(这一章没接)。所以墙钟的「活时间」不靠周期重读 RTC,而靠 boot 基线 + HPET 走过的 delta 推进(下面 clock_gettime)。RTC 只负责「开机那一刻的真世界时间」,之后交给 HPET 的精度。

## 接到 sys_clock_gettime

两个源备齐,接到 `sys_clock_gettime` 的两个 clock(`sys_clock_gettime.cpp`):

- **`CLOCK_MONOTONIC`** → 读 HPET:`g_hpet.monotonic_ns()`(boot-relative 纳秒)。HPET 没起来就退化到 PIT(粗)。
- **`CLOCK_REALTIME`** → RTC 的 `boot_epoch_seconds_`(秒级,粗)+ HPET 自开机走过的 delta(纳秒,精)。RTC 没读到就退化成 monotonic 值。

```cpp
// CLOCK_MONOTONIC: HPET 自开机走过的 ns
if (clock == CLOCK_MONOTONIC) return g_hpet.monotonic_ns();
// CLOCK_REALTIME: RTC boot epoch(秒)+ HPET delta(ns)
if (clock == CLOCK_REALTIME) return rtc_boot_epoch * 1'000'000'000ULL + g_hpet.monotonic_ns();
```

这个组合的妙处:墙钟的「秒」精度来自 RTC(开机那一刻读的),「秒以下」的纳秒精度来自 HPET(一直走的计数器)——两个硬件各贡献自己擅长的部分,凑出一个又准又精的墙钟,还不用周期重读 RTC。

## 验证

B 档,验证靠机制测 + host 纯算术 + boot 日志。

**第一层:host 纯算术单测。** `test/unit/test_hpet_math.cpp` 五例 `ticks_to_ns`(100MHz/1ns tick/跑一年不溢出);`test/unit/test_rtc_math.cpp` 六例(bcd 解码 + epoch 已知日对照:1970/2000/2024/闰日/时分秒累加)。这层是「算术门」——纯函数 host 可测。

**第二层:kernel 机制测。** `test_hpet.cpp` 几例,关键的 `test_counter_advances`(读两次主计数器,断言递增)——这条是防「绿盖住冻死设备」的哨兵:要是 ENABLE 没真生效(计数器没走),读两次是同一个值,测就红。还有 period sane(0 < period ≤ 1e8 fs)、monotonic 非减。`test_rtc.cpp` 读真实日期(sane year/month/day/h)+ boot epoch ∈ [2024, 2100)。

**第三层:boot 日志。** `make run` 打 `[HPET] MMIO ... period ... counter enabled` + `[RTC] <真实日期> (epoch ...)`,production boot 真把两个时钟都起来了。

`run-kernel-test-all` 两腿各 1012 passed / 0 failed(999 基线 + 13 HPET/RTC 测)。

## 这章没做的

- **HPET 的定时中断(timer)**:这一章只用它的 free-running 计数器(monotonic)。HPET 还能做「到点产生中断」(给 TCP 重传 RTO、调度抢占那种),那要配 comparator + IRQ,留 follow-up。
- **RTC 周期重同步**:boot 只读一次,墙钟靠 RTC 基线 + HPET delta。周期重读 RTC 对齐墙钟(防漂移)要 RTC 更新 IRQ,留 follow-up。
- **墙钟可设置**(`settimeofday`):这一章只读 RTC,不能写(调时间)。

## 小结

- 内核要两种时间:monotonic(boot-relative,测间隔)和墙钟(Unix epoch,真世界)。两个语义,两个硬件源。
- **HPET** 当 free-running 计数器:ACPI 找基址(0xFED00000)→ MMIO 映射 → 置 ENABLE → 记 boot 基线 → `monotonic_ns = ticks_to_ns(period, now - boot)`。两个实机坑:Config 在 `0x010`(间距 0x10)、一律 32-bit 访问(QEMU 丢 64-bit 写,主计数器两半读 + rollover)。`ticks_to_ns` 拆 hi/lo 避溢出,host 可测。
- **RTC** 读墙钟:0x70/0x71 端口(关 NMI)→ BCD→binary → Hinnant `days_from_civil` 算 epoch。防撕裂(UIP + 两读一致)、12h/BCD 读 Status B 自适应。boot 只读一次缓存基线。
- **clock_gettime 接线**:MONOTONIC 读 HPET;REALTIME = RTC boot epoch(秒)+ HPET delta(纳秒),两个硬件各贡献擅长的精度,不用周期重读 RTC。
- 诚实边界:HPET 只做计数器没做定时中断;RTC 不周期重同步、不可设置。
