---
title: 01 · per-CPU 架构与 LAPIC IPI:给第二个核备好发令枪
---

# per-CPU 架构与 LAPIC IPI:给第二个核备好发令枪

> 这是 SMP 弧的关键一步,但**不是**"让第二个核跑起来"那一步。本章做两件事:一是把 per-CPU 数据/结构从静态全局迁到 GS-based per-CPU 控制块(单核重构,行为不变,为多核铺地基)——这部分源码在上一章 050 打底,本章把它讲透;二是把 LAPIC 的 IPI(核间中断)接口立起来,这是 BSP 唤醒 AP 的**发令枪**(INIT-SIPI-SIPI 协议的 BSP 侧)。至于把 AP 从实模式拉到长模式、真跑起来的 trampoline,是下一章 052 的事——它和多核调度紧耦合(trampoline 把 AP 拉起后,AP 入口要立刻对接 per-CPU 调度器),单独拎出来编译不过。B 档:`test_apic` 验证 IPI 的 ICR 写入(`send_init`/`send_sipi`/`send_ipi`)。

## 这章咱们要点亮什么

1. **per-CPU 控制块**:`PerCpu` 结构,`%gs` 锚定本 CPU 的块;`percpu()` 读 `MSR_GS_BASE`。
2. **swapgs 纪律**:内核态/用户态切换时正确换 GS——这一步的坑比设计预想的大。
3. **per-CPU GDT/TSS**:`gdt_blocks[kMaxCpus]`,每核独立。
4. **LAPIC IPI 发令枪**:`send_init`/`send_sipi`/`send_ipi`,AP 唤醒协议(INIT-SIPI-SIPI)的 BSP 侧;trampoline 本体留 052。

## per-CPU 架构(单核重构,承 050)

把原来散落的静态全局(`g_per_cpu` 等)迁到 **GS-based per-CPU 控制块**。这段重构的源码随 050 落地(`percpu.hpp`/`percpu.cpp` + `gdt.cpp` 的 per-CPU 化),本章展开讲清楚为什么这么改。`PerCpu` 结构第一个成员是 `kernel_stack`(在 offset 0,因为 syscall 入口 `%gs:0` 取它)。`percpu()` 读 `MSR_GS_BASE` 拿到本 CPU 块的地址——内核态任意上下文(含中断)都安全。

一个**根本性的改变**:GS 从"per-task"变成"per-CPU"。以前 `context_switch` 会存/取 GS(每个 task 自己的 GS);现在 GS 是**每 CPU 一个**(指向本 CPU 的 PerCpu 块),context_switch **不再碰 GS**(`context_switch.S:61` —— "GS base is PER-CPU, not per-task")。FS(TLS)仍然 per-task(046 的 `fs_base` 不变)。

### swapgs 纪律(设计低估的硬约束)

这一步最有价值的发现:**原设计文档低估了 swapgs 的牵连**。问题在于:只有 `syscall.S` 做 swapgs,而 `interrupts.S`(ISR)**没有**。于是中断从用户态进来时,GS_BASE=0(用户态值),可 `schedule()`→`percpu()` 跑在中断上下文、读 `MSR_GS_BASE` 读到 0——炸。

所以必须先建**完整的 swapgs 纪律**,不止 syscall:

- **syscall entry/exit**:swap GS(syscall 从用户态进,GS 要切到内核值);
- **ISR 条件 swapgs**:中断入口按 `CS` 判 CPL——从用户态(CPL=3)进来的才 swap,内核态产生的中断不 swap(否则 swap 两次回原状出错);
- **jump_to_usermode**:切用户态时 swap。

不变量:内核态 `MSR_GS_BASE`=本 CPU PerCpu 块、`MSR_KERNEL_GS_BASE`=0(用户态值)。每个进/出内核的路径都维护这套。

> 这是"设计稿核对代码基线"的价值:光看设计文档会以为 swapgs 只是 syscall 的事;核对才发现 ISR 没做,是个隐藏的硬约束。所以 SMP 这种复杂改动,先出设计稿核对、再动手,比照着设计直接写稳。

### per-CPU GDT/TSS

`gdt_blocks[kMaxCpus]`(`gdt.cpp:17`):每 CPU 一个 GDT + TSS。`tss_set_rsp0` 走 `percpu()->cpu_id` 选本 CPU 的 TSS,签名不变但内部按 CPU 分——这样两个核的 `rsp0` 不互踩。

## AP boot 协议:IPI 发令枪(本章)+ trampoline(留 052)

多核启动有一条固定协议,Intel SDM 写死的:BSP(0 号核)通过 LAPIC 的 ICR(Interrupt Command Register)给 AP 发一串 **INIT → delay → SIPI → delay → SIPI**,把 AP 从 halt 唤醒。AP 收到 SIPI 后,从 SIPI vector 指向的实模式地址(物理地址 = vector × 4KB)开始执行——那里得放着一段把 AP 从 16 位实模式拉到 64 位长模式的 **trampoline**。

本章只落协议的 **BSP 发令侧**——三个 IPI 接口(`local_apic.hpp`):

- `send_init(dest)`(L110):ICR low = `kIcrModeInit | kIcrLevelAssert`,通知 AP "准备启动";
- `send_sipi(dest, vector)`(L113):ICR low = `vector | kIcrModeSipi | kIcrLevelAssert`,vector=0x08 → AP 从物理 0x8000 起跑;
- `send_ipi(dest, vector)`(L104):普通固定向量 IPI(多核调度的 reschedule 用它,052)。

ICR 是 64 位、分两个 32 位寄存器:high(L45,`kRegIcrHigh=0x310`)放目的 APIC ID(bits 24-31),low(L44,`kRegIcrLow=0x300`)放向量 + 投递模式 + flags。写 low 触发发送。

**为什么 trampoline 留到 052**:trampoline 不是一段孤立的启动代码。它把 AP 从实模式拉到长模式之后,AP 立刻要回答一个问题——"我跑什么任务?"。而这正是多核调度的核心(per-CPU run queue + 任务在核间迁移)。两者紧耦合:trampoline 的 AP 入口要对接 per-CPU 调度器,调度器又要假设 AP 已经在线、有自己的 run queue。把这套硬拆成"先 trampoline、后调度"两步,中间态既编译不过(AP 入口引用的调度符号还没就位)、也跑不起来(AP 上线了却没有可拉的 run queue)。所以本章的边界划在"IPI 发令枪就绪":BSP 已经**能**发出正确的 INIT-SIPI-SIPI(`test_apic` 用 MockMmio 验证 ICR 写入);但真按下去 AP 能不能跑、`-smp 2` 能不能看到第二个核 online,要等 052 把 trampoline 和调度一起落地。

## 已知局限(留 follow-up)

- **NMI/#DB 在 syscall-exit swapgs 窗口**(swapgs 后→SYSRET 前,GS_BASE=0 但仍内核态):此刻 NMI handler 调 `percpu()` 会读 0。窗口极窄、NMI 罕见、handler 不调 percpu,实际风险极低。Linux 的 paranoid NMI 路径留后续。
- **lost-wakeup**(futex/waitpid/mutex):单核不暴露,留多核调度(052)。
- **AP 还跑不起来**:本章只有 IPI 发令枪,没 trampoline。`-smp 2` 此刻看不到第二个核 online——那是 052。

## 验证

```bash
grep -rn 'PerCpu\|percpu()\|gdt_blocks\|swapgs\|send_init\|send_sipi\|send_ipi' kernel/arch/x86_64/ kernel/proc/ kernel/drivers/apic/ | grep -vE '\.o:' | head
grep -n 'send_init\|send_sipi\|send_ipi\|kRegIcr\|kIcrMode' kernel/drivers/apic/local_apic.hpp
```

构建 + 内核测试:

```bash
cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
cmake --build build --target run-kernel-test   # test_apic 里 IPI 三测(send_init/send_sipi/send_ipi)全绿
```

B 档端到端:`test_apic` 用 MockMmio 假装 LAPIC 寄存器,验证 `send_init`/`send_sipi`/`send_ipi` 写出的 ICR 值(high=dest<<24,low=vector|mode|assert)正确。**真"-smp 2 双核 online"要等 052 trampoline 落地**——本章结束时 AP 还没被唤醒。

## 小结与下一站

per-CPU 架构 + swapgs 纪律 + IPI 发令枪全到位,BSP 已经能发出正确的 INIT-SIPI-SIPI。第二个核还差最后一段:**trampoline 把它从实模式拉到长模式 + `ap_main` 接入调度**——这段在 052,和多核调度一起落地。

下一站 **052** 接上 **AP boot trampoline + 多核调度**:trampoline 实现 + per-CPU run queue + reschedule IPI + 真把 user task 迁移到 AP 上跑——SMP 真正"干活"的一步。
