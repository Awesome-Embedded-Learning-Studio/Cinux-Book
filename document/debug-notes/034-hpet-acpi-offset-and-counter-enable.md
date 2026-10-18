---
title: HPET absent：ACPI 字段错位、显式 MMIO 映射与主计数器使能
---

# HPET absent：ACPI 字段错位、显式 MMIO 映射与主计数器使能

2026-10-18，对应 pack-23 §4h。单核 x86_64、QEMU 11.1.2、SeaBIOS、
`-accel kvm`、AHCI/ext2。接手时的候选结论是「pc 默认 hpet=off，
SeaBIOS 仍发布 HPET 表，设备 MMIO 返回垃圾」。本次直接复验推翻了它：
默认 `pc` 的 HPET 已开启，真正的问题先出在表字段偏移。

## 修复前现场：读到的是物理地址 4

QMP `qom-get /machine hpet` 三组查询：

| machine 参数 | hpet 属性 | FindTable("HPET") | 原驱动 |
| --- | --- | --- | --- |
| `pc` | true | `0xffff800007fe22cf` | absent |
| `pc,hpet=on` | true | `0xffff800007fe22cf` | absent |
| `pc,hpet=off` | false | nullptr | absent，正常回退 |

`-machine pc,help` 列出 `hpet=<bool>` 只能证明存在开关，不能推出默认值。
本机默认值经 QMP 实测为 true；上游也在启用 CONFIG_HPET 时默认开启，
且只在发现 HPET 时生成对应 ACPI 表，可对照
[QEMU pc.c](https://github.com/qemu/qemu/blob/v9.2.0/hw/i386/pc.c) 与
[ACPI 构建代码](https://github.com/qemu/qemu/blob/v9.2.0/hw/i386/acpi-build.c)。
`intel-hpet` 不是合法 `-device` 名称，与机器是否已经提供 HPET 是两回事。

修复前的两个 on 组，都在 FindTable 返回后抓到完整 56 字节真表。
表头长度 0x38、OEM 为 BOCHS/BXPC；表体为：

```text
offset 0x24: 01 a2 86 80                         # Event Timer Block ID
offset 0x28: 00 00 00 00                         # GAS 前四字节
offset 0x2c: 00 00 d0 fe 00 00 00 00             # GAS address = 0xfed00000
offset 0x34: 00 00 00 00                         # number / minimum tick / attributes
```

GAS 位于 36 字节表头和 4 字节 block ID 之后，因此 address space 在
**+0x28**，8 字节地址在 **+0x2c**。旧代码分别用了 +0x2c、+0x30：
前者碰巧读到地址最低字节 0，后者把地址高半与尾字段拼成 0。
旧代码还只拼了地址的六个字节。

```text
GAS_SPACE_28=0 ADDRESS_2C=0xfed00000 WRONG_ADDRESS_30=0
WRONG_CAPS_PHYS_4=0xf000ff53
```

所以旧 period 实际来自 `DirectMapVirt(0 + 4)`，即
`0xffff800000000004`，内容是 BIOS 实模式中断向量表中的值。
**本案没有发生对 HPET MMIO 的正确读取**。合法域检查确实拒绝了假值，
但不能据此认定设备仿真未开。此次 RSDP 为 0xffff8000000f64e0，
位于固件扫描窗；上述读数全部来自本轮独立启动和当前 ELF。

## 修复：解析、映射、使能各补一层

- `hpet_layout.hpp` 按规范偏移读取 GAS，用已有 ReadWord64 解完整地址；
  检查至少 56 字节、system memory、非零及 1KiB 对齐。
- Hpet::init 检查地址落在当前 512GiB ioremap 预留范围内，然后调用已有
  `mm::MapDeviceWindow(block, 1024)`，通过 `IoremapVirt` 访问。
  caps/period 用对齐的 volatile 32 位读，counter 用 volatile 64 位读。
- 要求 capability 的 COUNT_SIZE_CAP 为 64 位；32 位计数器暂回退 PIT，
  避免把约 43 秒一次的 100MHz 回绕当作超大时间跳跃。
- 对 +0x10 的 General Configuration 做 32 位读改写：置 ENABLE_CNF，
  清 LEG_RT_CNF，保留其余位。读回确认 enable，再保存 boot_count。
  零计数是有效起点，不再以 `counter != 0` 判存在；失败前保持 present=false。
- 不配置 comparator 或 IRQ，PIT 继续提供 100Hz 调度心跳。驱动初始化需要
  使能写，运行时的时钟读取面只读；此阶段承接 QEMU/固件的未武装 comparator。

主计数器使能是独立的第二道问题：设备存在不等于 counter 已运行。
修复后的独立 KVM，在映射返回后、enable 写之前直接读到：

```text
BEFORE_ENABLE_CONFIG=0 COUNTER=0
HPET_INIT_RESULT=1
COUNTER_PTR=0xffff8180fed000f0 PERIOD=10000000 BOOT_COUNT=4035
CAPS=0x9896808086a201 CONFIG=0x1 COUNTER=8859
DIRECT_HPET_UNMAPPED
```

默认 pc 与显式 on 均成功，无 absent 行；off 没有 HPET 表，init=false，
串口照常提示回退并进入 shell。boot_count 与后续 counter 不同，确认已走时。

规范依据是 [Intel HPET 1.0a](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/software-developers-hpet-spec-1-0a.pdf)
§3.2.4 的表布局、§2.3.4 的 period、§2.3.5 的 ENABLE_CNF。
现有合法域 **0 < period <= 100,000,000fs** 正确，保持不变。
QEMU 的实际 period 为 10,000,000fs，即 10ns；不能把可移植检查收窄为仅此一值。

## 映射方案与工具参数裁决

采用仓内已有的正式设备窗路径，无需另造 ioremap，也不扩大 boot 的 framebuffer 门。
MapDeviceWindow 以 2MiB 页、cache-disable 标志铺设备片段，HPET 使用
`0xffff8180fed00000` 的 ioremap 别名；在创建用户地址空间前完成，
后续地址空间继承内核上半区映射。

本次 GDB 证实 `0xffff8000fed00004` 不可读。boot 早期 framebuffer 对应的
1GiB 设备门属于交接页表；内核 BringUpAddressSpace 重建 direct map 后，
不能把它当作 HPET 的常驻映射契约。旧代码没 #PF 的原因是错读了物理 4。

后续若要缩小权限和覆盖面，应在 MapDeviceWindow 统一补 4KiB UC/NX 映射、
重叠与范围检查、属性兼容和 TLB 刷新约定，让 AHCI/HPET 共用；
当前这轮只复用已有 2MiB 设备窗，不把通用映射器扩建混入驱动修复。

`scripts/mcp/qemu.py`、`gdb.py` **未改**。建议作者可显式固定
`-machine pc,hpet=on`，用于声明教学机型的设备约定，但这不是本案修复条件。
若固定机型意味着兼容性承诺，可再固定经验证的 pc-i440fx 版本；
同时保留 `hpet=off` 用例验证回退。本轮用默认、on、off 三组证明这一区别。

## 时钟实弹与换算溢出

测试使用新下载的官方 BusyBox 1.38.0，现有 musl 工具链静态编译，
开启 CONFIG_FEATURE_DATE_NANO，关闭 PREFER_APPLETS/SH_STANDALONE/SH_NOFORK。
旧 `/tmp` 构建已不存在，故不把本次二进制冒充 032/033 中的同一构建。
所有磁盘均复制到独立目录，通过 GDB 注入 `ash -c`，标准 ext2.img 未替换。

独立 musl 探针循环直接调用 clock_gettime(REALTIME/MONOTONIC)、
gettimeofday 和 time，检查尾数范围、单调性、秒值一致、至少前进 30ms，
以及 fine/coarse 两种预期。每组另执行八次 `busybox date +DATE=%s.%N`。

| 组 | clock 最小正采样差 | clock 亚毫秒样本 | gettimeofday 亚毫秒样本 | 结果 |
| --- | ---: | ---: | ---: | --- |
| 默认 pc | 7,290ns | 1025/1025 | 1024/1025 | fine PASS |
| pc,hpet=on | 7,800ns | 1026/1026 | 1023/1026 | fine PASS |
| pc,hpet=off | 10,000,000ns | 0/68877 | 0/68877 | coarse PASS |

on 的输出示例：

```text
CLOCK_MONOTONIC=0.101330430 CLOCK_REALTIME=1792300793.101337840 GETTIMEOFDAY=1792300793.101344
DATE=1792300793.162046230
DATE=1792300793.229659060
CLOCK_PROBE PASS fine
```

off 的 date 尾数依次为 .180000000、.230000000、.280000000 等，全部整 10ms。
三组父 ash 都退出为 0。**10ns 是计数器分辨率，7.8µs 是本轮调用采样差**，
不能把 BusyBox 两次进程执行的间隔解释为计时器分辨率。
time 接口本身只有秒，gettimeofday 只有微秒；纳秒输出来自 clock_gettime。

原 `(ticks * period_fs) / 1e6` 在 QEMU 100MHz 下约 5.12 小时后中间积溢出。
改为商余分解后相加，保持向下取整语义：

```text
(ticks / 1e6) * period + ((ticks % 1e6) * period) / 1e6
```

结果在 uint64 纳秒可表示的约 584 年内不因中间积提前回绕。
宿主测试覆盖六小时样本、非整数纳秒 period，以及真实 56 字节表快照、
完整 64 位地址、短表/非 MMIO/空基址/未对齐和 period 边界。

## 验收与复跑

- 构建开启 -Werror；四配置完整脚本退出 0，无编译 warning/error。
- `ctest --test-dir build/test --output-on-failure`：67/67
  （53 个宿主目标 + 14 个内核目标）。新增 hpet 目标含 4 个测试。
- test_kernel：普通与 UBSan/lockdep 各 14/14，全部 `-accel kvm`。
- acceptance：普通 53+14，ASan/泄漏 53，TSan 53，UBSan/lockdep 53+14，全绿。
- clang-tidy -p build：hpet.cpp、acpi.cpp、init_sequence.cpp、sys_clock.cpp、
  test_hpet.cpp 均退出 0，无项目诊断；新头文件由相关翻译单元一并检查。
  同时清理 acpi 的 unsigned shift 和 sys_clock 的直接 include/尾逗号诊断。

```sh
cmake --build build --target boot_image test_host test_kernel -j 6
ctest --test-dir build/test --output-on-failure
scripts/checks/acceptance.sh
clang-tidy -p build kernel/driver/hpet/hpet.cpp kernel/driver/acpi/acpi.cpp \
  kernel/init/init_sequence.cpp kernel/syscall/sys_clock.cpp test/unit/test_hpet.cpp
```

首次 acceptance 的四组测试已绿，但最后 boot_image 链接暴露 build-checks
遗留零字节目标文件。只清理该构建目录下 64 个空 `.o` 后，完整重跑脚本通过；
没有为此改源文件或放宽检查。沙箱内不暴露 /dev/kvm，本轮 QEMU 和含机内测试的
验收均在获准的沙箱外执行，没有 TCG 替代。

现场文件：修复前 `/tmp/cinux-hpet-before-{8aiw81i2,_8blyvoz,bv3zrzck}/`；
修复后默认/on/off 为 `/tmp/cinux-hpet-after-{tgmky0gj,28c4g5q1,epjdi_jf}/`；
时钟实弹分别为 `/tmp/cinux-hpet-clock-{7nv_4347,vxmvi_p0,atgqsg6g}/`。
可在本机重跑 `/tmp/cinux-hpet-audit.py`（仅适用于修复前 ELF）、
`/tmp/cinux-hpet-after-audit.py` 和 `/tmp/cinux-hpet-clock.py`；
探针源码 `/tmp/cinux-hpet-clock-probe.c`，用现有 musl-gcc 加 `-static -O2` 编译。
这些 /tmp 工件不是仓库长期回归资产，长期锁点是 test_hpet 的规范快照与数值测试。

最终日志：`/tmp/acceptance_station23_hpet_retry.log`、`/tmp/cinux-hpet-ctest.log`、
`/tmp/cinux-hpet-tidy.log`、`/tmp/cinux-hpet-clock-results.log`、
`/tmp/cinux-hpet-after-audit.log`。
