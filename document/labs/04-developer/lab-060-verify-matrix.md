---
title: Lab 060 · 验证基建(SMP 唤醒 + 首故障捕获)验证
---

# Lab 060 · 验证基建(SMP 唤醒 + 首故障捕获)验证

> 对应 `document/book/04-developer/060-verify-matrix.md`。验证档 **B 档**:本章是验证/调试基建,没有用户可见功能。验证两件事——`-smp 2` 那条腿**真的唤醒了 AP**(不再是空转)、故障 dump 的 gated 钩子和 debugcon 捕获**真在源码里且生产字节不变**。

## 目标

确认四件事:

1. `run-kernel-test-all` 统一入口在(单核 + `-smp 2` 两条腿一条命令);
2. `-smp 2` 腿**真探到 2 个 CPU** 且 **M3-2 AP 唤醒测试 engage**(不是空转);
3. gated 钩子 `g_ap_test_selfcheck_fn` 在源码里、默认 `nullptr`(生产字节不变);
4. 首故障捕获 `capture_first_gp/pf` 在 `handle_gp/pf` 的**最前面**(panic 之前)。

## 步骤

### 1. SMP 不再空转(-smp 腿真探到 2 CPU)

这是本章的核心验证。跑 SMP 腿,看它有没有真探到拓扑 + engage AP 唤醒测试:

```bash
cmake --build build --target run-kernel-test-smp 2>&1 | grep -aiE "M3-1|cpu_count|AP.*online|M3-2" | head
```

应看到 `[F-VERIFY M3-1] SMP topology: cpu_count=2 apic_id[0]=0 apic_id[1]=1`(真扫了固件 ACPI 表、探到 2 个 CPU)和 `[F-VERIFY M3-1] SMP mode (2 CPUs) -- M3-2 AP-wake tests engage here`(AP 唤醒测试在这条腿跑)。**对比修复前**:修复前 `-smp 2` 腿 `cpu_count` 恒 0(`boot_aps()` 直接 single-CPU 返回)、AP 从不醒——套件 BSP-only 跑,所有 SMP-only bug 抓不到。

> 想一条命令跑两条腿(防忘跑 SMP 变体),用统一入口:`cmake --build build --target run-kernel-test-all`(`qemu.cmake:355`)——先单核、再 `-smp 2`。CI 和默认验证都该切到这个。

### 2. gated 钩子(生产字节不变的契约)

```bash
sed -n '120,125p' kernel/arch/x86_64/ap_main.cpp
sed -n '178,195p' kernel/arch/x86_64/ap_main.cpp
```

应看到 `g_ap_test_selfcheck_fn = nullptr`(:122,默认空),以及 AP 在 `g_aps_online++` 之后、调度器自旋之前那段 `if (g_ap_test_selfcheck_fn != nullptr) { fn(cpu_id); for(;;) cli;hlt; }`(:189)。关键契约:**`fn=nullptr` 时整块跳过,生产二进制一字节不变**;只有测试内核往这个全局写函数指针,AP 才跑回读。这就是「生产代码接测试钩子但不污染生产」的 gated 范式。

钩子的回读结果槽 + magic(smp.hpp):

```bash
grep -n "kApSelfcheckMagic\|ApSelfcheckResult\|g_ap_selfcheck_results" kernel/arch/x86_64/smp.hpp
```

应看到 `kApSelfcheckMagic = 0xA5C0FFEE`(:60)、`g_ap_selfcheck_results[]`(:72)——AP 把回读(CR4/EFER/LSTAR)写进槽、最后置 magic,BSP 轮询 magic 判断「这个 AP 跑完了」(x86 TSO 保完整槽,不用 fence)。

### 3. 首故障捕获(debugcon,在 panic 之前)

```bash
sed -n '176,198p' kernel/arch/x86_64/exception_handlers.cpp
```

应看到 `handle_gp` 进来**第一件事**就是 `capture_first_gp(frame)`(:179),`handle_pf` 同理 `capture_first_pf`(:197)——在 `panic()`/`current()` 这些可能递归崩的调用**之前**。看 dump 走的通道:

```bash
grep -n "kDebugconPort\|0xE9\|FIRST #GP\|debugcon_str" kernel/arch/x86_64/fault_diag.cpp | head
```

应看到 `kDebugconPort = 0xE9`(:24,QEMU debug console,输出落 `build/debug.log`)和 `>>> FIRST #GP rip=...`(:59)。debugcon 是纯 IO 通道(`out %al,$0xE9`),不经内存/调度器,永远活着——所以哪怕后续 `kprintf` 自己 #PF、`%gs` 损坏连环 #GP,**第一个 fault 的 rip/rsp 已经稳稳落进 debug.log**,不被递归崩覆盖。`capture_first_gp/pf` 只捕获一次(递归 frame 跳过)。

## 验收清单

- [ ] `run-kernel-test-smp` 出 `cpu_count=2` + `M3-2 AP-wake tests engage`(SMP 不再空转)。
- [ ] `run-kernel-test-all` 统一入口在(`qemu.cmake:355`)。
- [ ] `g_ap_test_selfcheck_fn` 默认 `nullptr`(`ap_main.cpp:122`),gated `if` 在 :189——`fn=null` 生产字节不变。
- [ ] `capture_first_gp/pf` 在 `handle_gp/pf` 最前面(:179/:197),走 debugcon(0xE9→debug.log)。

## 别做这些

- **别**以为 `run-kernel-test-smp` 带 `-smp 2` 就测了 SMP——修复前它从不唤醒 AP,套件 BSP-only 跑。看 `cpu_count=2` + M3-2 engage 才算真测了 AP 路径。
- **别**在 `handle_gp/pf` 里把 `capture_first_*` 放到 `panic()`/`current()` 之后——那些调用可能递归崩(`%gs` 坏、`kprintf` 自己 #PF),会把首故障现场覆盖。首故障 dump 必须在任何可能递归的代码之前。
- **别**给生产代码插测试钩子用 `#ifdef`——用 gated 函数指针(`fn=null` 默认、生产字节不变)更干净,不用切两半。
- **别**把 gated 钩子当银弹往简单函数上插——它有「生产文件多了段测试才走的逻辑」的代价,只给 ap_main 这种「最该测又最难测」的高危路径用。
- **别**忘了这一章是基建不是功能——验证靠「SMP 腿真 boot AP」+「debuglog 留住首故障」+ grep 钩子,不是用户可见现象。
