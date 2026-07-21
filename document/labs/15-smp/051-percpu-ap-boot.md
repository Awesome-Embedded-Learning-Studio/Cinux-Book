---
title: Lab 051 · per-CPU 与 AP boot 验证
---

# Lab 051 · per-CPU 与 AP boot 验证

> 对应 `document/book/15-smp/051-percpu-ap-boot.md`。验证档 **B 档**(-smp 2 启动日志 2 CPU online)。验证靠构建 + 测试 + grep + 真机 -smp 2。

## 目标

确认四件事:

1. per-CPU 控制块在(`PerCpu`/`percpu()`/`gdt_blocks[kMaxCpus]`),GS 锚定本 CPU 块;
2. swapgs 纪律完整(syscall + ISR 条件 + jump_to_usermode);
3. AP boot trampoline 在(IPI INIT-SIPI-SIPI + `ap_trampoline` + `ap_main`);
4. `-smp 2` 真机启动 2 CPU online。

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

### 3. AP boot trampoline

```bash
grep -rn 'ap_trampoline\|send_init\|send_startup\|ap_main\|0x8000' kernel/arch/x86_64/ kernel/drivers/apic/ | grep -vE '\.o:' | head
```

去看 trampoline 的实模式→长模式切换(16→32→64)+ ap_main 的 per-CPU 接入(设 KERNEL_GS_BASE/gdt_blocks[cpu]/cpu_id)。

### 4.(B 档)真机 -smp 2

需要 `-smp 2` QEMU(改 qemu.cmake 或手动)。启动日志(grep -a)应见:AP 经 trampoline 到 ap_main、online;BSP 跑到 GUI。**此刻 AP 在 idle halt(没接调度),真双核跑任务是下一章 052。**

## 验收清单

- [ ] 构建 `build=0`,测试全绿(单核行为不变,869/0 贯穿)。
- [ ] PerCpu/percpu()/gdt_blocks[kMaxCpus]/swapgs 纪律在。
- [ ] AP boot trampoline(ap_trampoline/send_startup/ap_main)在。
- [ ] `-smp 2` 真机 2 CPU online。

## 别做这些

- **别**给 ISR 无条件 swapgs——要按 CS 判 CPL=3 条件 swap,否则内核态中断 swap 两次出错。
- **别**在 context_switch 里还存/取 GS——GS 是 per-CPU 不是 per-task(F4-M3 P1-2)。
- **别**指望 AP 启动后自动跑任务——AP 只是 online+halt,多核调度是 052。
