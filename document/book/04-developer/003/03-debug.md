---
title: 03 · 验证与收尾
---

# 验证与收尾

## 验证

验证靠机制测 + host 纯算术 + boot 日志。

**第一层:host 纯算术单测。** `test/unit/test_hpet_math.cpp` 五例 `ticks_to_ns`(100MHz/1ns tick/跑一年不溢出);`test/unit/test_rtc_math.cpp` 六例(bcd 解码 + epoch 已知日对照:1970/2000/2024/闰日/时分秒累加)。这层是「算术门」——纯函数 host 可测。

**第二层:kernel 机制测。** `test_hpet.cpp` 几例,关键的 `test_counter_advances`(读两次主计数器,断言递增)——这条是防「绿盖住冻死设备」的哨兵:要是 ENABLE 没真生效(计数器没走),读两次是同一个值,测就红。还有 period sane(0 < period ≤ 1e8 fs)、monotonic 非减。`test_rtc.cpp` 读真实日期(sane year/month/day/h)+ boot epoch ∈ [2024, 2100)。

**第三层:boot 日志。** `make run` 打 `[HPET] MMIO ... period ... counter enabled` + `[RTC] <真实日期> (epoch ...)`,production boot 真把两个时钟都起来了。

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
