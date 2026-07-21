---
title: 051 · per-CPU 与 AP boot
---

# 051 · per-CPU 架构与 AP boot:让第二个核跑起来

> 这是 SMP 弧**最难的一步**。两阶段:**Phase 1** 把 per-CPU 数据/结构从静态全局迁到 GS-based per-CPU 控制块(单核重构,行为不变,为多核铺地基);**Phase 2** 写 AP boot trampoline,把第二个核(Application Processor)从实模式一路拉到长模式、跑起来。B 档:`-smp 2` 启动日志能看到 2 CPU online。

## 这章咱们要点亮什么

1. **per-CPU 控制块**:`PerCpu` 结构,`%gs` 锚定本 CPU 的块;`percpu()` 读 `MSR_GS_BASE`。
2. **swapgs 纪律**:内核态/用户态切换时正确换 GS——这一步的坑比设计预想的大。
3. **per-CPU GDT/TSS**:`gdt_blocks[kMaxCpus]`,每核独立。
4. **AP boot trampoline**:INIT-SIPI-SIPI + `ap_trampoline @0x8000` + `ap_main`,AP 经 16→32→64 到长模式。

## Phase 1:per-CPU 架构(单核重构)

把原来散落的静态全局(`g_per_cpu` 等)迁到 **GS-based per-CPU 控制块**。`PerCpu` 结构第一个成员是 `kernel_stack`(在 offset 0,因为 syscall 入口 `%gs:0` 取它)。`percpu()` 读 `MSR_GS_BASE` 拿到本 CPU 块的地址——内核态任意上下文(含中断)都安全。

一个**根本性的改变**:GS 从"per-task"变成"per-CPU"。以前 `context_switch` 会存/取 GS(每个 task 自己的 GS);现在 GS 是**每 CPU 一个**(指向本 CPU 的 PerCpu 块),context_switch **不再碰 GS**(`context_switch.S:61` —— "GS base is PER-CPU, not per-task")。FS(TLS)仍然 per-task(046 的 `fs_base` 不变)。

### swapgs 纪律(设计低估的硬约束)

这一步最有价值的发现:**原设计文档低估了 swapgs 的牵连**。问题在于:只有 `syscall.S` 做 swapgs,而 `interrupts.S`(ISR)**没有**。于是中断从用户态进来时,GS_BASE=0(用户态值),可 `schedule()`→`percpu()` 跑在中断上下文、读 `MSR_GS_BASE` 读到 0——炸。

所以 Phase 1 必须先建**完整的 swapgs 纪律**,不止 syscall:

- **syscall entry/exit**:swap GS(syscall 从用户态进,GS 要切到内核值);
- **ISR 条件 swapgs**:中断入口按 `CS` 判 CPL——从用户态(CPL=3)进来的才 swap,内核态产生的中断不 swap(否则 swap 两次回原状出错);
- **jump_to_usermode**:切用户态时 swap。

不变量:内核态 `MSR_GS_BASE`=本 CPU PerCpu 块、`MSR_KERNEL_GS_BASE`=0(用户态值)。每个进/出内核的路径都维护这套。

> 这是"设计稿核对代码基线"的价值:光看设计文档会以为 swapgs 只是 syscall 的事;核对才发现 ISR 没做,是个隐藏的硬约束。所以 SMP 这种复杂改动,先出设计稿核对、再动手,比照着设计直接写稳。

### per-CPU GDT/TSS

`gdt_blocks[kMaxCpus]`(`gdt.cpp:17`):每 CPU 一个 GDT + TSS。`tss_set_rsp0` 走 `percpu()->cpu_id` 选本 CPU 的 TSS,签名不变但内部按 CPU 分——这样两个核的 `rsp0` 不互踩。

## Phase 2:AP boot trampoline

Phase 1 把 BSP(bootstrap CPU,0 号核)改造成"多核就绪"。Phase 2 启动 AP(其余核):

- **IPI**:BSP 经 LAPIC 的 ICR 给 AP 发 **INIT-SIPI-SIPI`(send_init`/`send_startup`);
- **`ap_trampoline @0x8000`**:一段实模式代码,AP 收到 SIPI 后从这里开始,经 **16→32→64** 切到长模式;
- **`ap_main`**:AP 进长模式后的 C 入口,设自己的 `KERNEL_GS_BASE=&percpu_blocks[cpu]`、加载 `gdt_blocks[cpu]`、填 `cpu_id`/`apic_id`,然后 online + halt。

trampoline 有几个汇编坑(记进 CinuxOS 的 GOTCHA):**GAS 宏不内联展开**,地址计算用内联表达式 `(label-start+0x8000)`;**CR3 切换要在 higher-half 的 `ap_entry_long`**(0x8000 切完页表后映);AP `cli;hlt` 不碰共享调度器(多核调度是下一章)。

真机日志:`-smp 2` 启动,AP 经 trampoline 到 `ap_main`,设 GS/GDT/IDT/LAPIC,online + halt;BSP 续跑到 GUI。**两个核都 online 了**。

## 已知局限(留 follow-up)

- **NMI/#DB 在 syscall-exit swapgs 窗口**(swapgs 后→SYSRET 前,GS_BASE=0 但仍内核态):此刻 NMI handler 调 `percpu()` 会读 0。窗口极窄、NMI 罕见、handler 不调 percpu,实际风险极低。Linux 的 paranoid NMI 路径留后续。
- **lost-wakeup**(futex/waitpid/mutex):单核 + AP idle 不暴露,留多核调度(052)。

## 验证

```bash
grep -rn 'PerCpu\|percpu()\|gdt_blocks\|swapgs\|ap_trampoline\|send_startup\|ap_main' kernel/arch/x86_64/ kernel/proc/ | grep -vE '\.o:' | head
grep -rn 'kMaxCpus' kernel/
```

构建 + 内核测试 + 真机 `-smp 2`:

```bash
cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
cmake --build build --target run-kernel-test
# 真机双核(需 -smp 2,看启动日志 2 CPU online)
```

B 档端到端:`-smp 2` 启动,日志见 AP 经 trampoline 到 `ap_main`、online;BSP 跑到 GUI 不炸。此时 AP 还在 idle halt(没接调度),真"两核跑任务"是下一章 052。

## 小结与下一站

真双核 online 了:per-CPU 架构 + swapgs 纪律 + AP boot trampoline 全到位。但 AP 此刻只是"醒着 halt",没跑任务。

下一站 **052** 接上**多核调度**:per-CPU run queue + reschedule IPI + 真把 user task 迁移到 AP 上跑——SMP 真正"干活"的一步。
