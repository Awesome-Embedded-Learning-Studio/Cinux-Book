---
title: 04 · 迁移 GP 与并发债清算
---

# 迁移 GP 与并发债清算

## 迁移 GP:一个藏得很深的致命顺序

前面几步都对了,AP 也 online 了,可一旦真把一个用户任务迁移到 AP 上跑,立刻 `#GP`。这是个折磨人的 heisenbug,根因藏在 GDT 加载的顺序里。

长模式下,`%fs`/`%gs` 的段基址**不在 GDT 描述符里,而在 MSR**(`MSR_FS_BASE`/`MSR_GS_BASE`)——`%fs` 装 per-thread 的 TLS,`%gs` 装本核的 per-CPU 块。问题出在 `GDT::load`(`gdt.cpp`):它加载 GDT 后会重载 DS/ES/SS,而老版本**顺手也重载了 `%fs`/`%gs`**——加载一个平坦数据选择子到 `%gs`,会把描述符的 base(0)塞进 `MSR_GS_BASE`,**把 GS 锚点清零**。

BSP 不出事,是因为它 boot 时 GDT 加载在设 GS 之前。AP 是反的——看 `ap_main` 的顺序:先 `write_msr(GS_BASE, percpu)`(第 1 步锚定 GS),**再** `gdt_blocks[cpu].init()`→`GDT::load()`(第 2 步)。这一 load,刚锚好的 GS_BASE 被清零。此后 AP 上每个 `percpu()`、`current()` 都去读 `MSR_GS_BASE`,拿到 0,于是从物理 0x18 那种地方读到 BIOS 留的垃圾指针,一解引用——非规范地址,x86-64 上产生的是 **`#GP`(err=0),不是 `#PF`**。

修法分两半。一是**根治**:`GDT::load` 干脆不重载 `%fs`/`%gs`(长模式下它们就该是 null 选择子 + MSR base,Linux 也这么干)。二是**加固 first-fault 捕获**:这种 #GP 本身会递归——`handle_gp` → `panic` → `Scheduler::current()` 读 `%gs:24` → GS_BASE 非规范使这次解引用又 #GP → `handle_gp` 再来,无限循环。结果 panic 打出来的寄存器是递归帧的,真正的出错 RIP 丢了。所以 `handle_gp` 顶部、在碰任何 `%gs` 之前,先用 `rdmsr`/`io_outb`(这俩都不依赖 `%gs`)把第一现场——faulting RIP、CR3、实时的 GS_BASE/KERNEL_GS_BASE——裸打到 debugcon。一个 once-flag 防递归帧刷屏。这是永久加固:往后任何 %gs-corrupt 的 #GP,都留着真 RIP 而不是一串读不懂的递归崩。

> (一处源码注释还是旧状态:`scheduler.hpp` 里一句"AP 还不跑用户任务,因为迁移会 GP"是修这个 GP **之前**写的,修完忘了更新。以 `ap_idle_entry` 的实际实现为准——它就是上面那个 `schedule()+sti;hlt` 循环,AP 真拉任务跑。)

## AP 真跑线程后清算的并发债

AP 一旦真跑起线程,一批单核时代不是 bug 的代码立刻变成真 bug——因为现在真有两个核并发了。

**原子引用计数**。`SharedCwd`(CLONE_FS 共享的 cwd)和 `SharedSigActions`(CLONE_SIGHAND 共享的信号处理表)是引用计数的共享对象。单核时 `++refcount`/`--refcount` 没事;两核同时 clone/exit,一加一减并发跑,丢更新——要么 use-after-free(计数先归零了还有人用),要么泄漏(计数没归零对象却没人引用了)。修法是把 `acquire`/`release` 换成 `__atomic_add_fetch`/`__atomic_sub_fetch`(ACQ_REL),并且去掉「先读 refcount>0 再操作」那种 racy 预检查(规范的原子引用计数不预检查)。顺带核对:`FDTable`(CLONE_FILES)本来就用自旋锁护着 refcount,已经 SMP 安全,不动它,不折腾。

**锁序图死锁检测**(opt-in,`CINUX_LOCKDEP`)。把原来只数「持锁跨 schedule 深度」的 lockdep 升级成真的死锁检测器(`lockdep.cpp`):每核一个持锁栈(按 `cpu_id` 索引——顺带修了旧版用全局计数器的 SMP 缺陷,两核共踩一个计数器)、一张全局的锁序邻接图(边 `from→to` = 持 from 时取 to)、取锁时 DFS 检环。`Spinlock::acquire`/`release` 调进/出钩子,`schedule()` 里"持锁跨切换会死锁"的断言改读 per-CPU 深度。默认 OFF(header 里是 inline no-op,零开销),开了才检测。注一对坏锁序能准确 panic,还原能继续跑。
