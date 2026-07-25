---
title: 04 · 收尾:验证 + 下一站 + 参考
---

# 收尾:验证 + 下一站 + 参考

## 验证

调度逻辑(队列轮转、入队出队、`CpuContext` 布局、`TaskBuilder` 字段)在 host 上镜像着测。[test_scheduler.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_scheduler.cpp) 把 `RoundRobin`、`Scheduler`、`TaskBuilder`、`CpuContext` 的逻辑在 host 侧重写了一份(不链内核代码,`-O2` 编、`CINUX_HOST_TEST` 门控),盯这些:空/满/单任务队列的 `pick_next`、`dequeue` 中间项、`TaskBuilder` 的字段默认值与 null entry 守卫、`CpuContext` 的 `sizeof` 和各偏移:

```bash
ctest --test-dir build -R scheduler --output-on-failure
```

真正的 `context_switch`(真汇编换栈)和真正的任务构造(真 PMM/VMM/Heap 出栈)只能在 QEMU 里验。[test_scheduler.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_scheduler.cpp) 在机内跑一连串场景:`TaskBuilder` 能造出合法任务(tid 从 1 起、state=Ready、`ctx.rip` 指向入口、栈非 0——这一项用的是真 PMM/VMM/Heap 建出来的栈),null entry 返回 nullptr,`init` 注册默认 `RoundRobin`,`RoundRobin` 的 enqueue/dequeue/pick_next 轮转与出队中间项,`CpuContext` 布局(`sizeof==64`、各 offset);而专门验 `context_switch` 本身的那一项用的是两个**裸 `CpuContext` + 静态栈缓冲区**(不经过 `TaskBuilder`),纯测汇编换栈能不能让两个上下文来回切、状态在 Ready/Running 间正确流转:

```bash
cmake --build build --target run-big-kernel-test
```

机内会打 `[SCHED] Scheduler initialised with RoundRobin class`,test section `Scheduler/Process Tests (019)` 全过、末尾 `ALL TESTS PASSED`,就说明这套任务/切换/调度在真硬件语义下成立。

最后是**生产 demo** 本身的现象:直接跑大内核(`cmake --build build --target run`,或对应 QEMU 目标),串口应该看到 `thread_a` / `thread_b` 严格交替的 5 轮、各自一句 `done`、各自 `[SCHED] Task tid=N '...' exited`,最后 `No more tasks, halting.`——交替说明 `context_switch` 真的把 CPU 在两条流之间递来递去,干净退出说明「栈顶压 `exit_current`」那条设计是对的。

## 下一站

到这里,内核第一次有了「多条活着的执行流」。`Task` 把一条执行流的所有可挂起状态装进 64 字节;`context_switch.S` 用「存 callee-saved + 换栈 + 跳 rip」在它们之间瞬切;`RoundRobin` 轮流点名;higher-half 扶正让隔离地基稳了。

但你会发现 019 的痛:它是**协作式**的。线程要是不主动 `yield`,它就霸着 CPU 不放——`thread_a` 如果忘了调 `yield`,`thread_b` 永远没机会跑。真实的系统不能指望每个线程都自觉。下一站(020)就治这个:把调度器接到那个**已经在跑**的时钟中断上,让 `irq0` handler 在固定节拍打断当前线程、强制切走——也就是**抢占式**调度。那会引入新的难题(中断可以在任意指令处发生,不再是干净的函数边界;切走时要保存的现场更重;多个 CPU 各自的当前任务怎么管),于是 020 还会带来 `per_cpu` 和最基本的同步原语。019 的 `Task` 和 `context_switch` 是那一切的地基——地基先稳,上面才好盖。

---

### 参考

- **System V AMD64 ABI**([x86-psABIs/x86-64-ABI](https://gitlab.com/x86-psABIs/x86-64-ABI)):callee-saved 寄存器约定(`rbx`、`rbp`、`r12`–`r15`)——`CpuContext` 只存这 6 个 + `rsp`/`rip` 的全部依据;caller-saved 跨调用不保证存活,故无需保存。
- **xv6-riscv `swtch()`**([mit-pdos/xv6-riscv](https://github.com/mit-pdos/xv6-riscv)):同样的「只存 callee-saved + 换栈 + ret」手法,可对照 Cinux `context_switch` 的设计。
- **Intel SDM Vol.3**(本地 `document/reference/intel/SDM-Vol3A-*.pdf`):通用寄存器集、`rip`/`rsp` 如何定义执行流、规范地址(higher-half 的由来),可用 `pdf-reader` 搜 "general-purpose" / "canonical" 复核。
- 018 章 · [给每个世界一套页表:地址空间](../05-memory/004/):`AddressSpace` 的「内核半区 `PML4[256..511]` 共享、用户半区私有」设计——higher-half 收口之所以必要,就是为了对上这个设计。
- 本 tag 源码:[process.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/process.hpp) / [process.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/process.cpp)、[context_switch.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/context_switch.S)、[scheduler.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/scheduler.hpp) / [scheduler.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/scheduler.cpp)、[elf_loader.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/elf_loader.cpp)(`return saved_entry`)、[linker.ld](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/linker.ld)(`KERNEL_VMA`/`KERNEL_LMA`);测试 [test_scheduler.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_scheduler.cpp)(host 镜像)、[test_scheduler.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_scheduler.cpp)(QEMU 机内)。
