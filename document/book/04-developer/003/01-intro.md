---
title: 01 · 导引:两种时间,两个源
---

# 导引:两种时间,两个源

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
