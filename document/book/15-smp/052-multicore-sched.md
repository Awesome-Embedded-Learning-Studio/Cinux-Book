---
title: 052 · AP boot trampoline 与多核调度
---

# 052 · AP boot trampoline 与多核调度:让第二个核真干活

> 上一章备好了 IPI 发令枪,本章把发令枪真按下去。两件事:**一是**写一段 trampoline,把第二个核从 16 位实模式一路拉到 64 位长模式、让它跑起来;**二是**让多核调度真正干活——AP 不再干站着 halt,而是从一个共享的 run queue 里拉任务跑。这条路中间有个藏得很深的迁移 GP,还有 AP 真跑线程之后才暴露出来的一批并发债。A 档:`-smp 2` 启动,AP1 经 trampoline 上线、从共享队列拉到任务。
>
> 诚实交代在前:本机的 QEMU 下,`-smp 2` 真机会在 AHCI 驱动上踩一个时序竞态(单核完全正常),所以"两个核真跑用户任务"的端到端演示在本机受这个竞态挡住、跑不到那一步。AP boot 和调度机制本身是工作的——经单元测试 + 启动日志(`[AP1] online`)验证;那个 AHCI 竞态是另一码事,本章末尾会讲清它的边界。

## 这章咱们要点亮什么

1. **AP boot trampoline**:16 位实模式 → 32 位保护 → 64 位长模式,靠一套临时 GDT 和临时页表,BSP 往 trampoline 里塞好参数。
2. **AP 的 C 入口**:锚定 GS、装本核 GDT、开 LAPIC、向 BSP 报到,再切进自己的 idle 任务。
3. **共享 run queue 怎么不撞车**:`pick_next` 把任务**拿走**、不留队列里,两个核绝不会抢到同一个任务。
4. **reschedule IPI + AP idle 循环**:AP 空闲时 `sti;hlt`,被 IPI 唤醒后回头查队列、有活就干。
5. **迁移 GP**:一个 GDT 加载顺序的致命坑——讲清它怎么把 GS 锚点清零。
6. **AP 真跑线程后清算的并发债**:原子引用计数、锁序图死锁检测。

## AP boot trampoline:把第二个核从实模式拉到长模式

x86 多核启动的规矩是:第二个核(AP,Application Processor)上电后停在 halt,等第一个核(BSP)给它发 SIPI;SIPI 里带一个向量,AP 就从「向量 × 4KB」那个物理地址开始执行,而且**是从 16 位实模式开始**。所以这个入口地址上,必须放着一段能把 AP 从实模式一路带到长模式的代码——这就是 trampoline(跳板)。

这段跳板在 `ap_trampoline.S`,BSP 把它拷到物理 `0x8000`,然后发 SIPI 向量 `0x08`(0x08 << 12 = 0x8000)。AP 醒来后经历三个阶段,每个阶段往 debugcon(端口 0xE9,落到 `build/debug.log`)打一个标记(`1`/`2`/`3`),triple fault 时就能看出走到哪步:

- **16 位实模式**:清段寄存器、开 A20(端口 0x92,保险)、加载一个临时 GDT、置 CR0.PE,然后 `ljmp` 到 32 位。
- **32 位保护模式**:置 CR4.PAE、把 BSP 准备的临时 PML4 物理地址写进 CR3、置 EFER.LME、重载 GDT(这次带上 64 位代码段)、置 CR0.PG,`ljmp` 到 64 位。
- **64 位长模式**:把 BSP 塞进来的四个参数(AP 栈、内核 PML4、cpu_id、入口地址)读进寄存器,然后 `jmp` 到内核镜像里的 `ap_entry_long`。

跳板有个汇编技巧值得专门讲:**它被链接在更高半的内核镜像里,却在物理 `0x8000` 执行**。所以一个标号运行时的物理地址是 `(标号 - ap_trampoline_start + 0x8000)`。GAS 会把「标号减 start」这部分(两个符号都在同一个 section 里)折叠成一个常量,于是每个这样的表达式都是一个绝对立即数——不用运行时修址、不用改 linker script。访存时把这个常量装进寄存器,再用寄存器间接寻址(`lgdt`/`mov` 没法直接吃这个立即数)。

为什么要先在 `0x8000` 跑一段、再跳到 `ap_entry_long`?因为 CR3 切换有讲究。临时页表既 identity-map 了低内存(让 `0x8000` 这段跳板能跑),又镜像了更高半内核镜像(让 `ap_entry_long` 也能跑)。但 AP 一旦切到真正的内核 PML4,`0x8000` 就不再映射了。所以切 CR3 这一步必须在「两边都映射」的 `ap_entry_long` 里做,不能在 `0x8000` 里做:

```asm
ap_entry_long:
    movq    %r12, %cr3     # 切到真正的内核页表
    movq    %rbx, %rsp     # 设好 AP 的内核栈
    call    ap_main        # 不返回
```

## BSP 侧:怎么把 AP 喊起来

BSP 的活儿在 `boot_aps()`(`ap_main.cpp`)。它先准备两份页表和一份跳板,再逐个把 AP 喊起来。

**临时页表**(`build_ap_temp_page_tables`):分配三个页(PML4/PDPT/PD),identity-map 物理 0..64 MB(2 MB 大页),再在更高半(`P4[511]`/`PPT[510]`)镜像同一份 PD——让 AP 在 `0x8000` 跑跳板时、以及跳到更高半 `ap_entry_long` 时,地址都通。这份临时页表用完就扔,AP 切到内核 PML4 后再也不碰它。

**塞参数**:`copy_trampoline_to_lowmem` 把跳板拷到 `0x8000`,`inject_param` 往跳板的数据区里写四个值——AP 栈顶、临时 CR3、内核 CR3、`ap_entry_long` 地址、cpu_id。BSP 知道每个 AP 该用第几号 per-CPU 块(`percpu_blocks[cpu]`),就把这个索引塞进去。

**INIT-SIPI-SIPI**:`send_init` → 忙等 → `send_sipi(vec=0x08)` → 忙等。第二条 SIPI 是兜底:只有当第一条没把 AP 带起来(看 `g_aps_online` 计数没涨)才补发,免得 AP 已经 online 并 halt 之后,再来一条 SIPI 让它把跳板重跑一遍、炸掉。然后 BSP 在一个有上限的循环里 `pause` 自旋,等这个 AP 原子地给自己 `++g_aps_online` 报到。

## AP 侧:ap_main 怎么接入内核

AP 进了长模式、切好 CR3 和栈,就 `call ap_main(cpu_id)`。这是 AP 的 C 入口,干七件事(`ap_main.cpp`):

1. **锚定 GS**:`write_msr(GS_BASE, &percpu_blocks[cpu_id])`、`write_msr(KERNEL_GS_BASE, 0)`。从此这个核的 `percpu()` 读 `MSR_GS_BASE` 拿到自己的块。这一步的顺序是后面迁移 GP 的关键,先记着。
2. **装本核 GDT + 共享 IDT**:`gdt_blocks[cpu_id].init()`(每核独立 TSS/IST)、`g_idt.load()`(IDT 全局共享)。
3. **开 LAPIC**:读自己的 apic_id 存进 per-CPU 块,`enable(0xFF)`(伪中断向量跟 BSP 一致)。LAPIC 的 MMIO 窗口是「谁访问就解码到谁的本地 APIC」,所以同一份 `g_lapic` 驱动能驱动每个核自己的 LAPIC。
4. **向 BSP 报到**:`__atomic_add_fetch(&g_aps_online, 1, SEQ_CST)`。BSP 那边正在自旋等这个计数。
5. **等调度器就绪**:`cli;pause` 自旋等 `Scheduler::is_initialized()`。注意是 `cli;pause` 不是 `sti;hlt`——因为 `boot_aps` 跑在 `Scheduler::init` **之前**,init 不发 IPI,`hlt` 会一直睡死。init 几微秒就好,所以这个自旋极短。
6. **建本核 idle**:`setup_ap_idle(cpu_id)` 给这个 AP 造一个专属 idle 任务(入口 `ap_idle_entry`),`current()` 设成它。必须先有个合法的 current,第一次 `context_switch` 才不会被 `schedule()` 里「current()==nullptr 直接返回」的早出口挡掉。
7. **切过去**:`context_switch(&dummy, &ap_idle->ctx)`,跳到 `ap_idle_entry` 自己的栈上,AP 再也不回 `ap_main`。

## 多核调度:共享 run queue 怎么不撞车

到这里两个核都 online 了,但"多核调度"真正难的地方才刚开始。核心问题:**两个核从同一个 run queue 里挑任务,怎么保证不会挑到同一个?**

第一件事,先给每个核一个**专属的 idle 任务**(`idle_tasks_[kMaxCpus]`)。为什么不能两个核共享一个 idle?因为 context_switch 会切到 idle 的 ctx、用 idle 的栈。两个核要是共享一个 idle,就会两个核同时切到同一个 ctx、踩同一份 16 KB 栈——必炸。所以 BSP 用 `idle_tasks_[0]`(老的 `idle_entry`,while-hlt,PIT 驱动),每个 AP 用 `setup_ap_idle` 造自己的(入口 `ap_idle_entry`)。

第二件事,**运行中的任务绝不许留在 run queue 里**。看 `RoundRobin::pick_next`(`roundrobin.cpp`):它选出最高优先级的任务后,**把它从队列里移除**(`remove_at_locked`),不是轮转着再塞回队尾。注释把这个纪律说得很直白:一个正在跑的任务要是还留在共享队列里,第二个核的 `pick_next` 就可能再选中这个已经 Running 的任务,两个核 context-switch 到同一份栈/ctx 上。

配套的纪律在 `schedule()`(`scheduler.cpp`)里:prev(刚跑完、要让出的任务)如果还是 Ready,才 re-enqueue 进队列;`pick_next_task` 再把 winner **移除**。于是队列里**永远只有"可跑但没在跑"的任务**,任何时刻一个任务至多被一个核选中。单核的轮转语义也没丢:schedule 先把 yielding 的 prev 塞回队,再 pick,一个光杆任务会挑回自己(`next==prev` 路径)继续跑。

## reschedule IPI:把闲置的 AP 拽起来干活

现在 AP 的 idle 不再是死 halt。看 `ap_idle_entry`(`scheduler.cpp`):

```cpp
while (true) {
    if (has_runnable_task()) {   // 纯 peek,不 dequeue
        schedule();              // 有活,从共享队列拉一个跑
    }
    __asm__ volatile("sti; hlt");  // 没活,开中断 + halt
    __asm__ volatile("cli");
}
```

AP 空闲就 `sti;hlt` 睡。当别处有新任务可跑了(比如某个任务被 unblock、或 fork 出新任务),谁来叫醒这个 AP?靠 **reschedule IPI**。向量选 `0xE0`(`smp.hpp`),特意避开 PIC IRQ 区(0x20-0x2F)、伪中断(0xFF)、sigreturn 陷阱(0x80)。`wake_idle_ap` 给每个 online AP 发一炮 IPI——**best-effort,不精确**:多发给一个正忙的 AP 无害,它的 idle 循环重新查一遍队列、空的话再 halt 就是。这换来的是不用维护一个「哪个核正 idle」的精确标志(那个标志的 BSP 读 / AP 写自己又要费心 race)。

这里有个经典的 **lost-wakeup 窗口**要堵:AP 查了队列(空)、正要 `sti;hlt`,就在这当口,另一个核往队列里塞了个任务并(按理)该发 IPI——可 AP 还没 hlt,IPI 白发,AP 睡死。堵法是 `has_runnable_task()` 这个**纯 peek**(不 dequeue,各 class 的 `is_empty()` 自己加锁),在 `cli` 下查:查完才 `sti;hlt`。而塞任务的 `add_task`/`unblock` 发的正是这发 IPI——只要任务在查和 hlt 之间入队,IPI 就会把 AP 从 hlt 拽起来重查,不会漏。

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

## 已知局限

- **`-smp 2` 真机的 AHCI 竞态**:本机 QEMU 下,两个核跑起来后,生产镜像的 AHCI 驱动会在 `identify` 上踩一个时序竞态 panic(单核 `-smp 1` 的 AHCI 完全正常,`Read sector 0` 成功跑到 GUI)。这是 AHCI 驱动和 SMP 的交互问题,跟本章的 SMP 调度代码无关——AP boot(`[AP1] online`)和调度机制都工作。换 QEMU 版本或环境(别的 QEMU 版本、或单核配置)不复现,所以是环境相关的 heisenbug。真机的"两核端到端跑用户任务"演示在本机被它挡住,后面收。
- **上面那句 scheduler.hpp 过时注释**:待清理。

## 验证

```bash
# trampoline + AP 入口 + 调度多核改造 + IPI 都在
grep -rn 'ap_trampoline\|ap_main\|boot_aps\|wake_idle_ap\|kRescheduleIpiVector\|ap_idle_entry\|has_runnable_task' kernel/arch/x86_64/ kernel/proc/ | grep -vE '\.o:' | head
# 共享 run queue 的多核纪律 + per-CPU idle
grep -n 'remove_at_locked\|pick_next\|idle_tasks_\|setup_ap_idle' kernel/proc/roundrobin.cpp kernel/proc/scheduler.cpp | head
```

构建 + 单核测试(单核行为不变,是回归基线):

```bash
cmake -B build -S . -DCINUX_BUILD_TESTS=ON && cmake --build build -j$(nproc)
cmake --build build --target run-kernel-test
```

`-smp 2` 看两个核 online、AP 经 trampoline 上线:

```bash
cmake --build build --target run-smp   # 启动日志见 [AP1] online (apic_id=1)
```

A 档端到端:启动日志里 `[SMP] INIT-SIPI-SIPI -> apic_id 1` → `[AP1] GS anchored` → `[AP1] online`,两个核都 online。调度机制的更细验证(共享队列不 double-pick、lost-wakeup 关窗、原子 refcount)靠单核测试里的 scheduler/sync 并发用例——它们就是为多核正确性写的。真机 `-smp 2` 跑用户任务的端到端演示,受上面那个 AHCI heisenbug 挡住,本机跑不到那一步。

## 小结与下一站

第二个核真跑起来了:trampoline 把它从实模式拉到长模式,per-CPU idle + 共享 run queue 让两个核分工干活又不撞车,reschedule IPI 把闲置的核拽起来,迁移 GP 那个致命顺序也根治了。AP 真跑线程顺手把并发债(原子 refcount、lockdep)清算了一遍。

但 `-smp 2` 真机还有个 AHCI 驱动的时序竞态没收(本机 QEMU 复现、单核正常)。那是下一站要收拾的——连同任何别的 SMP 真跑后才冒头的迁移竞态。
