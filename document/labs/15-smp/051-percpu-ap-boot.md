---
title: Lab 051 · per-CPU 与 LAPIC IPI 验证
---

# Lab 051 · per-CPU 与 LAPIC IPI 验证

> 对应 `document/book/15-smp/051-percpu-ap-boot.md`。验证档 **B 档**(`test_apic` 验证 IPI 的 ICR 写入)。本章只到 IPI 发令枪,trampoline/AP 真跑是 052。验证靠构建 + 测试 + grep。

## 目标

确认四件事:

1. per-CPU 控制块在(`PerCpu`/`percpu()`/`gdt_blocks[kMaxCpus]`),GS 锚定本 CPU 块;
2. swapgs 纪律完整(syscall + ISR 条件 + jump_to_usermode);
3. LAPIC IPI 接口在(`send_init`/`send_sipi`/`send_ipi`,ICR 写入);
4. `test_apic` 里 IPI 三测全绿。

## 步骤

### 1. 构建 + 测试

```bash
git checkout 051_smp_percpu_ap_boot 2>/dev/null || git checkout 051_*
cmake -B build -S . && cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
cmake --build build --target run-kernel-test
```

### 2. per-CPU + swapgs

```bash
grep -rn 'PerCpu\|percpu()\|gdt_blocks\|swapgs' kernel/arch/x86_64/ kernel/proc/ | grep -vE '\.o:' | head
grep -n 'context_switch.S\|PER-CPU' kernel/arch/x86_64/context_switch.S | head
```

**思考**:为什么 GS 从 per-task 改成 per-CPU?——见章节:每 CPU 一个 PerCpu 块(本 CPU 的 kernel_stack/current 等),context_switch 不再碰 GS。FS(TLS)仍 per-task。**swapgs 纪律为何比设计预想复杂?**——ISR 原来无 swapgs(只 syscall.S 有),中断从用户态进入 GS_BASE=0 而 percpu() 在中断上下文读 → 必须给 ISR 加条件 swapgs(按 CS 判 CPL=3)。这是设计稿核对代码才抓到的硬约束。

### 3. LAPIC IPI 发令枪

```bash
grep -rn 'send_init\|send_sipi\|send_ipi\|kRegIcr\|kIcrMode' kernel/drivers/apic/local_apic.hpp kernel/drivers/apic/local_apic.cpp | head
```

去看 ICR 的写法:high=`kRegIcrHigh`(0x310)放 dest<<24,low=`kRegIcrLow`(0x300)放 vector|mode|assert;`send_init`/`send_sipi`/`send_ipi` 三个接口分别对应 INIT、SIPI(vector=页号)、固定向量。`test_apic.cpp` 用 MockMmio(普通 RAM 假装 MMIO 窗口)验证这些写入。

### 4.(B 档)test_apic 的 IPI 测试

```bash
cmake --build build --target run-kernel-test 2>&1 | grep -A2 'APIC'
```

应见 `test_send_init_writes_icr`、`test_send_sipi_writes_vector`、`test_send_ipi_fixed` 全过(ICR high/low 写入正确)。**注意**:本章没有 trampoline,`-smp 2` 此刻看不到第二个核 online——AP 真跑是下一章 052。

## 验收清单

- [ ] 构建 `build=0`,测试全绿(单核行为不变,贯穿)。
- [ ] PerCpu/percpu()/gdt_blocks[kMaxCpus]/swapgs 纪律在。
- [ ] LAPIC IPI 接口(send_init/send_sipi/send_ipi + ICR 常量)在。
- [ ] `test_apic` 的 IPI 三测全绿。

## 别做这些

- **别**给 ISR 无条件 swapgs——要按 CS 判 CPL=3 条件 swap,否则内核态中断 swap 两次出错。
- **别**在 context_switch 里还存/取 GS——GS 是 per-CPU 不是 per-task。
- **别**指望 051 结束时 `-smp 2` 能看到第二个核——trampoline 在 052,本章只有 IPI 发令枪。
