---
title: Lab 003 · HPET/RTC 时钟验证
---

# Lab 003 · HPET/RTC 时钟验证

> 对应 `document/book/04-developer/003/`。验证档 **B 档**:这一章交付的是两种时钟(monotonic + 墙钟)+ clock_gettime 接线,内部机制。验证靠 host 纯算术单测(换算/解码)+ kernel 机制测(计数器真在走)+ boot 日志(真实时间)。

## 目标

确认六件事:

1. **两种时钟语义**:monotonic(HPET,boot-relative)vs 墙钟(RTC,Unix epoch);
2. **HPET free-running 计数器**:ACPI 找基址 → MMIO → ENABLE → boot 基线 → monotonic_ns;
3. **HPET 两个实机坑**:Config 在 `0x010`、一律 32-bit 访问(QEMU 丢 64-bit 写);
4. **`ticks_to_ns` 溢出安全**(拆 hi/lo,host 可测);
5. **RTC 读墙钟**:0x70/0x71 端口、BCD→binary、Hinnant `days_from_civil`、UIP 防撕裂;
6. **clock_gettime 接线**:MONOTONIC=HPET,REALTIME=RTC epoch + HPET delta。

## 步骤

### 1. host 纯算术单测

```bash
./build/test/test_hpet_math && ./build/test/test_rtc_math
```

应看到 `test_hpet_math` 5 passed(`ticks_to_ns`:100MHz、1ns tick、跑一年不溢出等)+ `test_rtc_math` 6 passed(bcd 解码 + epoch 已知日对照:1970=0 / 2000-01-01=946684800 / 2024-01-01=1704067200 / 闰日 / 时分秒累加)。这层是「算术门」——纯函数 host 可测,跟内核环境无关。

### 2. HPET:ticks_to_ns + init

```bash
sed -n '51,57p' kernel/drivers/hpet/hpet.hpp
```

应看到 `ticks_to_ns(period_fs, ticks)` —— **溢出安全拆 hi/lo**:`(ticks/divisor)*period + (ticks%divisor)*period/divisor`,朴素乘法 100MHz 跑一年会溢出 uint64。再看 init:

```bash
sed -n '85,100p' kernel/drivers/hpet/hpet.cpp
```

应看到 `period_fs_ = read32(GeneralCaps + 4)`(period 在 Caps **高 32 位**)+ `write32(GeneralConfig, read32(...) | ENABLE_CNF)`(置 ENABLE 让计数器走)。`init()` 流程:ACPI 找基址 → MMIO 映射(FLAG_PCD uncached)→ 读周期 → ENABLE → 捕获 boot_counter 基线。

### 3. HPET 的两个实机坑

```bash
grep -nE "kHpetRegGeneralConfig|0x010|0x0F0|high1|high2|rollover" kernel/drivers/hpet/hpet.cpp | head
```

应看到 Config 寄存器偏移 `0x010`(HPET 通用寄存器**间距 0x10**,不是 0x008——0x008 落 reserved 写了全丢)+ 主计数器 `0x0F0` 两半读(`read32(0x0F0)`+`read32(0x0F4)`)的 rollover 保护(`high1`/`high2` 不一致就重读 `low`,标准无 race 算法)。这两个都是「读对写错/写不生效」实机教训出来的——QEMU 丢 64-bit MMIO 写,故一律 32-bit。

### 4. RTC:BCD + days_from_civil

```bash
sed -n '52,72p' kernel/drivers/rtc/rtc.hpp
```

应看到 `bcd_to_binary`(0x26→26)+ `days_from_civil`(Howard Hinnant proleptic Gregorian,精确无溢出)+ `datetime_to_unix_seconds`(= days*86400 + h*3600 + m*60 + s)。都是 inline 纯函数,host 可测。再看端口读:

```bash
grep -nE "0x70|0x71|outb|inb|0x80|UIP|Status" kernel/drivers/rtc/rtc.cpp | head
```

应看到 `outb(0x70, 0x80|index)`(bit7 关 NMI 防 read 中途踩 RTC)→ `inb(0x71)`;读 Status A 的 UIP(更新进行中)轮询 + 两读一致(防「读一半日期更新了」的撕裂);读 Status B 判 BCD/binary + 12/24h 自适应。

### 5. clock_gettime 接线

```bash
sed -n '38,58p' kernel/syscall/sys_clock_gettime.cpp
```

应看到 `CLOCK_MONOTONIC` → `g_hpet.monotonic_ns()`(HPET 自开机走过的 ns);`CLOCK_REALTIME` → RTC boot epoch(秒,粗)+ HPET monotonic delta(纳秒,精)。HPET 没起来 MONOTONIC 退化 PIT;RTC 没读到 REALTIME 退化 monotonic。这个组合:墙钟秒精度来自 RTC(开机读一次),纳秒精度来自 HPET(一直走)——两个硬件各贡献擅长的,不用周期重读 RTC。

### 6. 机制测 + boot 日志

机制测关键的 `test_counter_advances`(读两次主计数器,断言递增)是「防绿盖住冻死设备」的哨兵——ENABLE 没真生效(计数器没走)读两次同值就红:

```bash
cmake --build build --target run-kernel-test 2>&1 | grep -iE "hpet|rtc|counter" | head
```

应看到 HPET/RTC 各项 PASS(period sane、counter advances、monotonic 非减、RTC 读 sane 日期 + boot epoch)。

boot 日志(test kernel 不走 init,要起真内核):

```bash
cmake --build build --target run
```

应看到 `[HPET] MMIO 0xFED00000, period 10000000 fs (100000000 Hz), counter enabled` + `[RTC] <真实日期> (epoch ...)`——两个时钟都真起来了,RTC 读的日期跟宿主机一致。

两腿汇总:

```bash
cmake --build build --target run-kernel-test-all 2>&1 | grep -E "Tests: [0-9]{4} passed" | tail -1
```

应看到单核 + `-smp 2` 两腿各 `1012 passed, 0 failed`(999 基线 + 13 HPET/RTC 测)。

## 验收清单

- [ ] `test_hpet_math` 5 passed + `test_rtc_math` 6 passed(纯算术 host 单测)。
- [ ] `hpet.hpp:54` `ticks_to_ns` 溢出安全拆 hi/lo;`hpet.cpp:99` ENABLE Config。
- [ ] HPET Config 偏移 `0x010`(间距 0x10);主计数器 `0x0F0` 两半读 + rollover;一律 32-bit(QEMU 丢 64-bit 写)。
- [ ] `rtc.hpp:52` bcd_to_binary + `:58` days_from_civil(Hinnant);`rtc.cpp` 0x70/0x71 端口关 NMI + UIP 两读一致。
- [ ] `sys_clock_gettime.cpp` MONOTONIC=HPET,REALTIME=RTC epoch+HPET delta。
- [ ] 机制测 `test_counter_advances` PASS(计数器真在走);boot 日志 `[HPET] ...` + `[RTC] <真实日期>`;两腿 1012/0。

## 别做这些

- **别**把 monotonic 和墙钟混用一个源——monotonic 不能受墙钟拨动(测间隔会错),墙钟得是真世界时间(不能 boot-relative)。两种语义,两个源(HPET 给 monotonic,RTC 给墙钟)。
- **别**用 64-bit MMIO 写 HPET——QEMU(和部分真机)丢 64-bit 写,读回 0。一律 32-bit:period 读 Caps 高半、ENABLE 是 `write32(Config, read32(Config)|1)`、主计数器两半读 + rollover。
- **别**把 HPET Config 当 `0x008`——HPET 通用寄存器**间距 0x10**,Config 在 `0x010`(0x008 落 reserved 写了全丢,ENABLE 死活不生效)。QEMU 源码 `HPET_CFG` 宏是权威偏移。
- **别**手搓「年月日→天数」——闰年规则、月份天数容易错。用 Hinnant 的 `days_from_civil`(精确且 host 可验证)。也别假设 RTC 是 24h BCD——读 Status B 判模式,自适应。
- **别**以为 HPET 能定时中断——这一章只用了它的 free-running 计数器(monotonic)。「到点叫我」的 timer(comparator + IRQ,给 TCP RTO/调度抢占)没做,留 follow-up。RTC 也不周期重同步(boot 只读一次,墙钟靠 RTC 基线 + HPET delta)。
