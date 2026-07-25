---
title: 04 · 收尾:验证 + 下一站 + 参考
---

# 收尾:验证 + 下一站 + 参考

## 验证

先说 host 侧的覆盖现状,得如实:**020 没有新增独立的 host 单元测试**。019 那组镜像 `test/unit/test_scheduler.cpp`(测 `RoundRobin`/`Scheduler`/`TaskBuilder`/`CpuContext` 的纯逻辑)在 020 的 diff 里未改动,host 侧覆盖仍停留在那一组。020 新增的 `tick`/`schedule`/`block`/`unblock` 这些**没有 host 镜像**——它们要么依赖 `context_switch` 的真汇编换栈,要么依赖中断/PIT 的真硬件语义,只能在 QEMU 里验。

```bash
# host 侧(019 那组, 020 未变)
ctest --test-dir build -R scheduler --output-on-failure
```

真正的验证在 QEMU 机内。[test_scheduler.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_scheduler.cpp) 跑在真 PMM/VMM/Heap 之上,节名从 019 的 `(019)` 改成了 `(020)`,并在原有用例之后新增 Test 9 `test_scheduler_new` 三个用例:

```bash
cmake --build build --target run-big-kernel-test
```

机内 `TEST_SECTION("Scheduler/Process Tests (020)")` 应全过,其中新增三项分别盯:`test_is_initialized`(`init()` 后 `is_initialized()==true`)、`test_remove_task`(`add` 后 `remove`,state 变 `Dead`)、`test_block_unblock`(`add` 后 `block`→`Blocked`、`unblock`→`Ready`)。注意这三个是**状态机/接口**层面的验证,不触发真抢占——抢占只能靠下面的生产 demo 用肉眼验。

最后是**生产 demo**,这是抢占是否真生效的唯一肉眼验证。跑大内核:

```bash
cmake --build build --target run
```

串口应该看到 `[A]` / `[B]` / `[C]` … 六个线程被时钟中断**交错**打断——不再是 019 那种 A 整段跑完才轮到 B 的严格顺序,而是某个线程的忙循环跑到一半就被切走、换另一个线程的输出插进来、稍后再轮回来。每个线程最后各自打一句 `done`。如果你看到的不是交错、而是「只有第一个线程被抢占、其余顺序跑完」,那就是撞上了**案例二那条 IF 丢失的 bug**——`sti` 没接上,回去查 `context_switch.S`。

## 下一站

020 把切换挂到了时钟上,「谁让出 CPU」不再靠线程自觉。可代价也摆在眼前:调度器、就绪队列、PIT 计数器这些共享数据,**理论上**已经暴露在「被中断打断、又被新任务碰」的并发路径下了——虽然单核 + 中断门语义下「真并发」还没发生,但只要再多一个执行源(多核、或者中断里真去动队列),竞态就会冒头。而我们现在连一把锁都没真正用上。

下一站(021)就治这个:基于 020 备好的 `Spinlock`,把 `Mutex`、`Semaphore`、等待队列落地,让 `block`/`unblock` 真正派上用场(线程因为等 I/O、等锁而阻塞,被唤醒源重新 enqueue),并立刻对现有组件做一遍并发安全审查。`PerCPU` 也还要从「单核全局」长成「真 per-CPU」;至于更高半区的 ring3、系统调用、独立地址空间切换,再往后。020 的时钟 + `sti` + `idle` 是那一切的节奏地基——节奏先稳,上面才好盖并发。

---

### 参考

- **Intel SDM Vol.3A §6.12.1 "Exception- or Interrupt-Handler Procedures"**(本地 `document/reference/intel/SDM-Vol3A-System-Programming-Guide-Part1.pdf`,PDF 第 209 页 / 书内 6-13 页,已读到正文):进入 handler 时处理器把 `EFLAGS`/`CS`/`EIP` 压栈,特权级变化时 handler 栈「从当前任务的 TSS 获得」;`IRET`「把保存的标志恢复进 EFLAGS」。支撑「中断门进入时 IF 被清、`IRETQ` 还原 IF」与「`tss_set_rsp0` 的硬件依据」两条。
- **Intel SDM Vol.3A §6.12.1.3 "Flag Usage By Exception- or Interrupt-Handler Procedure"**(同 PDF,第 213 页 / 书内 6-17 页,已读到原文):「经中断门访问 handler 时,处理器清 IF 标志以防止其它中断干扰当前 handler……后续 IRET 把 IF 恢复为栈上保存值;陷阱门不影响 IF」。支撑「IRQ0 stub 一进去就是 IF=0」这条根因,以及案例二的非对称现象。
- **Intel SDM Vol.2B STI 的「延迟一拍」语义**(本地 `document/reference/intel/SDM-Vol2B-Instruction-Reference-M-U.pdf`,STUI 条目 PDF 第 691 页 / 书内 4-683 页,已读到正文):STUI 条目以对比方式写明「STI 的效果延迟一条指令」。支撑「`sti` 紧贴 `jmp`,换栈+跳转这一瞬不被中断劈开」的设计正确性。
- **GCC `__atomic` Builtins**(GCC 在线手册 `https://gcc.gnu.org/onlinedocs/gcc/_005f_005fatomic-Builtins.html`):`__atomic_test_and_set`(原子置 1 并返回旧值)、`__atomic_clear`(原子清 0)、`__ATOMIC_ACQUIRE`/`__ATOMIC_RELEASE` 内存序。支撑 `sync.hpp` 的 `Spinlock` 实现。
- **OSDev Wiki "Context Switching" / "Spinlock"**(`https://wiki.osdev.org/Context_Switching`、`https://wiki.osdev.org/Spinlock`,域名 200 在线):从中断 handler 里触发 schedule 的通用思路、`test_and_set` + `pause` 的自旋锁写法,概念性对照。
- **xv6-riscv**(仓库 `https://github.com/mit-pdos/xv6-riscv`):时钟中断在 trap 处理里触发 `yield` 的对照——切换点从函数边界挪到中断返回路径,与本章设计同源。
- **System V AMD64 ABI**(`https://gitlab.com/x86-psABIs/x86-64-ABI`):callee-saved(`rbx/rbp/r12-r15`)约定——`CpuContext` 只存这 6 个 + `rsp`/`rip`,而 `RFLAGS` 不在 callee-saved 之列,这正是「协作式 `context_switch` 本不碰 RFLAGS、抢占式才要补 `sti`」的根。延续 019 章已核引用。
- **001 章 · [让内核长出第二条执行流:进程上下文](../001/)**:`Task`/`context_switch`/`RoundRobin`/higher-half 地基,本章直接接续;`CpuContext` 布局与 callee-saved 论证亦出自此。
- 本 tag 源码:[scheduler.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/scheduler.hpp) / [scheduler.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/scheduler.cpp)、[context_switch.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/context_switch.S)、[gdt.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/gdt.hpp) / [gdt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/gdt.cpp)、[pit.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/pit/pit.cpp)、[per_cpu.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/per_cpu.hpp)、[sync.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/sync.hpp)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp);测试 [test_scheduler.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_scheduler.cpp)(QEMU 机内,节名 `(020)`)。
