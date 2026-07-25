---
title: 05 · 主线四 · 纵深期:IPI shootdown 与 deferred CoW 范式
---

# 主线四 · 纵深期:IPI shootdown 与 deferred CoW 范式

## 主线四 · 纵深期:IPI shootdown 与 deferred CoW 范式

这一阶段把视野从「单张共享表的锁」抬到「跨核 TLB 一致性」——SMP 上一个更难的问题。

### 病根:TLB 是每核私有的,页表是共享的

单核时代,改完页表(unmap / 改权限 / CoW 换页)本核一条 `invlpg`(`flush_tlb`)刷掉这条 TLB 映射就收工,别的核不存在,不存在 stale 缓存。CoW 换页时本地刷完直接 `pte_count_dec_and_test` 把旧页 free,天经地义。

SMP 时代不一样了:本核刷了没用——**别的核的 TLB 里那条旧映射还活着**。核 A 把某虚拟地址的 PTE 改掉、把旧物理页还给 buddy 之后,核 B 的 TLB 还缓存着旧映射,B 继续用旧物理地址读写——读到复用后的别人的数据、写错页(use-after-free)。所以释放旧页前,必须先**广播一个 IPI 给所有其他核,等它们都 `invlpg` 完、ack 回来**,才能 free。

### IPI shootdown 基建:广播 + ack 计数

机制本身很直白,是一次「广播 + ack 计数」的同步握手。发送方([tlb.cpp](../../../kernel/arch/x86_64/tlb.cpp#L34-L58)):

```cpp
void tlb_shootdown_page(uint64_t vaddr) {
    auto guard = g_shootdown.lock.guard();          // 单 in-flight:串行并发调用方
    g_shootdown.vaddr          = vaddr & ~0xFFFULL; // 页对齐
    g_shootdown.acks_remaining = online_ap_count(); // 除自己外在线的核数
    flush_tlb(g_shootdown.vaddr);                   // 本地先刷,不 IPI 自己
    if (g_shootdown.acks_remaining == 0) return;    // 单核短路
    drivers::apic::g_lapic.send_ipi_all_others(kShootdownIpiVector);  // 广播
    while (__atomic_load_n(&g_shootdown.acks_remaining, __ATOMIC_ACQUIRE) != 0) {
        __asm__ volatile("pause");                  // spin 等 ack 归零
    }
}
```

IPI 向量挑了 `0xE1`,紧挨 reschedule `0xE0`,刻意避开 PIC IRQ 段(`0x20-0x2F`)、spurious(`0xFF`)、sigreturn(`0x80`)([smp.hpp](../../../kernel/arch/x86_64/smp.hpp#L15-L22))。发送用 Local APIC ICR 的「all-excluding-self」简写(`bits[19:18]=11`),一口气发给所有其他核。

接收端([tlb.cpp](../../../kernel/arch/x86_64/tlb.cpp#L60-L67)):

```cpp
extern "C" void shootdown_ipi_handler(InterruptFrame* /*frame*/) {
    // ISR_IRQ stub owns the EOI;this handler just invalidates + acks.
    flush_tlb(__atomic_load_n(&g_shootdown.vaddr, __ATOMIC_RELAXED));   // invlpg 那条 vaddr
    __atomic_sub_fetch(&g_shootdown.acks_remaining, 1, __ATOMIC_ACQ_REL);
}
```

两个细节值得记住:**EOI 归 ISR_IRQ asm stub 统一做**(handler 自己不调 `eoi`),避免 APIC 模式下 PIC EOI 留 vector 抬高优先级冻住中断子系统;**vaddr 用 RELAXED 读合法**,因为 x86 TSO 下 LAPIC ICR MMIO 写对之前的普通 store 有序,接收端拿到 IPI 时发送方的 vaddr store 一定已可见,不需要显式 fence。

### 死锁推导:为什么不能直接挂到 CoW fault

这套同步握手有一个**致命的上下文限制**,也是它被设计成「deferred(延后)」的根本原因。

`handle_cow_fault` 跑在 `#PF` 中断门里,进入即 IF=0(中断门在入口清 IF)。如果在 IF=0 上下文里直接做同步 shootdown,**两核同时各自 CoW 会确定性互锁**——`-smp 2` 下这个场景确定性可复现:

1. 核 A 拿了 `g_shootdown.lock`,写下 vaddr、`acks_remaining=1`,发 IPI 给 B,开始 spin 等 B 的 ack;
2. 核 B 此刻也卡在 `#PF` 里、卡在 `g_shootdown.lock` 上 spin(IF=0);
3. 核 B 永远拿不到锁,永远不会处理 A 的 IPI;核 A 永远等不到 ack。

环形等待,死锁。**关键点**:`g_shootdown.lock` 是全局单 in-flight 锁、vaddr 槽也是全局单槽,所以死锁不挑物理页——两核 CoW 的是同一个共享页还是各自不同的页都一样会撞上这把全局锁(fork 后父子写共享页只是最常见的触发形态)。另一个关键点:`Spinlock::guard` 不动 IF(只有 `IrqGuard` 才 `cli`)——所以等锁时 IF 保持 0,hole 成立。光说「挪到 kthread」不够,必须说清楚为什么挪了就不死锁(下面讲)。

### 破局:换执行上下文,不是加锁

deferred 的正解不是「加一把更巧的锁」,而是**把 free 这一步从缺页路径里拆出去**。缺页路径只做两件无副作用的事:

1. `pte_count_dec_and_test_no_free` —— 减计数 + 跑审计(坏 free 瞬间 panic,审计门在原地保留),但**不真 free**(`pte_count` 已由 dec 减到 0;推迟的只是 `refcount` 那一笔的 `buddy_.free`,实物页留在 buddy 之外等 drain 兑现):

```cpp
bool PMM::pte_count_dec_and_test_no_free(uint64_t phys) {
    // ... pte_count -1,到 0 才转调 refcount_dec_and_test_no_free ...
}
bool PMM::refcount_dec_and_test_no_free(uint64_t phys) {
    // ... refcount -1 + 完整 audit(free-vs-pte / free-vs-cache,坏则 panic)...
    // NOTE: do NOT store pte_count=0 / buddy_.free here -- caller frees after shootdown.
    //       pte_count is already 0 (audit above).
}
```

（[pmm.cpp](../../../kernel/mm/pmm.cpp#L279-L289) 和 [pmm.cpp](../../../kernel/mm/pmm.cpp#L327-L350)。这就是 044 章那两本账的 `no_free` 变体——所有权账面归零,实物留着等 drain 兑现。）

2. 把 `{old_phys, vaddr}` 塞进一条 pending 链表 + 给信号量 `post` 一下([tlb.cpp](../../../kernel/arch/x86_64/tlb.cpp#L85-L113)):

```cpp
void enqueue_pending_shootdown(uint64_t phys, uint64_t vaddr) {
    if (!__atomic_load_n(&g_drain_active, __ATOMIC_ACQUIRE)) {
        cinux::mm::g_pmm.free_page(phys);   // drain 没起:退化为 inline free(单核/未启 drain)
        return;
    }
    // ... kmalloc 节点、挂 g_pending_head、g_pending_sem.post() ...
}
```

真正会死锁的那段——sync shootdown + buddy free——交给一个常驻的 drain 内核线程(IF=1,可被信号量阻塞后调度切走):

```cpp
void tlb_drain_entry() {
    while (true) {
        g_pending_sem.wait();   // 阻塞(调度让出,不是 sti/hlt)
        PendingShootdown* node;
        while ((node = dequeue_pending_shootdown()) != nullptr) {
            tlb_shootdown_page(node->vaddr);      // IF=1 下安全的同步 shootdown
            cinux::mm::g_pmm.free_page(node->phys); // NOW safe to free
            cinux::mm::kfree(node);
        }
    }
}
```

（[tlb_drain.cpp](../../../kernel/arch/x86_64/tlb_drain.cpp#L36-L47)。deferred 的兑现端。)

#### 死锁解除的两条论证

光说「挪到 kthread」不够,得说清楚为什么挪了就不死锁:

1. **drain 持的锁不是 fault 路径在 IF=0 里需要的**——PMM `lock_` 只在 `free_page` 内短持、`g_pending_lock` 短、`g_shootdown.lock` 单 in-flight,fault 路径在 IF=0 里一把都不碰;
2. **spin 等 ack 的终止条件成立**——drain spin 等 ack 时,目标核若正卡在 `#PF`(IF=0),`#PF` 一返回 IF 恢复它就会立刻服务 `0xE1` IPI 给出 ack,spin 有界、必然终止。

#### 信号量两半不对称

`post` 在 IF=0 安全、`wait` 在 IF=0 会崩——这是信号量的一个微妙不对称:

- `post` 用普通 guard(不动 IF)+ `Scheduler::unblock`(只改 run-queue、不 schedule);
- `wait` 会 `schedule_blocked` 切走,必须有 scheduler。

所以 fault 只 `post`、kthread 才 `wait`。一个 `g_drain_active` bool gate 把「无 scheduler 的测试内核」和「生产内核」统一到同一段代码:gate=false 时 enqueue 直接 inline `free_page`(无 shootdown,等同单核旧行为,零回归),gate=true 时才真 push。同一个 `enqueue_pending_shootdown` 函数在两种上下文都安全。

### 海森堡悖论(再提一次)与这条推导的分量

回到主线一讲过的海森堡悖论——deferred CoW 这套设计里,race-detect 默认 OFF 的理由又一次得到印证:竞态 timing-sensitive,插探针会改时序。deferred 的价值恰恰在于它**不靠时序侥幸**——它把「会互锁」从「可能发生」降到「结构上不可能」(fault 路径根本不做 shootdown),这是比「加锁 + 希望别踩进窗口」强得多的正确性保证。

这条「IF=0 里不能 spin 等跨核 ack」的死锁推导,是全章最值钱的一段叙事,它串起了 044 章的 `pte_count` / `refcount` 拆分——`no_free` 变体减计数但不释放,所有权账面归零而实物留着等 drain 兑现,正是这条推导逼出来的设计。把「正确性子集」拧成一根绳,这根绳的结就在这儿。
