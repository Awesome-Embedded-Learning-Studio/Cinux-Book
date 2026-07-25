---
title: 05 · dump_memory_stats:四条正交维度 + PF delta(rate 而非 total)
---

# dump_memory_stats:四条正交维度 + PF delta(rate 而非 total)

`dump_memory_stats`([diagnostics.cpp:26](../../../kernel/mm/diagnostics.cpp#L26))把「内存压力」拆成 PMM/slab/PageCache/#PF 四条正交数据维度(timestamp 行只是时间轴不算数据维度),每次同步报全,后来追加第 5 条 ext2 I/O,一条串口日志就是曲线上的一个点:

```cpp
// kernel/mm/diagnostics.cpp:26-74(节选关键行)
void dump_memory_stats() {
    const uint64_t free_pages  = g_pmm.free_page_count();
    const uint64_t total_pages = g_pmm.total_page_count();
    const uint64_t pf_total    = pf_count();

    // Boot-relative timestamp so the periodic stats log is a curve with a real
    // time axis (HPET monotonic ns) ...
    const uint64_t now_ns =
        cinux::drivers::g_hpet.available() ? cinux::drivers::g_hpet.monotonic_ns() : 0;
    kprintf("[MEM] === t=%llu.%03u s ===\n", ...);

    kprintf("[MEM] PMM:       %u / %u pages free ...\n", ...);          // 物理内存余量
    kprintf("[MEM] Slab:      %u slab pages mapped\n", ...);            // 内核对象分配器
    kprintf("[MEM] PageCache: %u cached (%u hits / %u misses)\n", ...); // 文件页缓存

    // Delta vs the previous dump so the periodic stats thread's log shows a PF
    // rate, not just a monotonic total.  static: dump_memory_stats has no
    // concurrent callers in practice (panic once + the single stats thread).
    static uint64_t last_pf = 0;
    kprintf("[MEM] #PF:       %u total (+%u since last dump)\n", ...);
    last_pf = pf_total;

    // B2.5: cumulative ext2 read I/O (count + bytes + wall time) with delta ...
    static uint64_t last_io_ns    = 0;
    static uint64_t last_io_reads = 0;
    // ... ext2_read_ns() / ext2_read_count() / ext2_read_bytes() + delta ...
}
```

五条行的设计:

- **`[MEM] === t=N.NNN s ===`**:HPET 时间戳。让曲线有真实时间轴,能把 [MEM] 样本对齐到 workload 阶段(比如 g++ 编译窗口)。HPET 不可用时打 0。
- **`[MEM] PMM: free/total pages`**:物理内存余量。看是不是快 OOM。
- **`[MEM] Slab: total_slab_pages`**:内核对象分配器占页。看是不是泄漏膨胀。
- **`[MEM] PageCache: cached_pages + hit/miss`**:文件页缓存占用与命中率。grow-only bug 在这里显形。
- **`[MEM] #PF: 累计缺页数 + (+Δ since last dump)`**:这是关键设计——用 `static uint64_t last_pf`([diagnostics.cpp:53](../../../kernel/mm/diagnostics.cpp#L53))跨调用算 delta。
- **`[MEM] I/O: ext2 read 累计 reads/bytes/ms + delta`**:后来追加的第 5 行,把「内存压力」扩展到「I/O 时间归属」——卡顿到底是 demand-paging 的 I/O 时间,还是 syscall/TCG 翻译开销。

## profiling 靠趋势不靠单点——static last_pf 是核心设计

#PF 用 delta 而不是单调总量,是这套设施的灵魂。dev note 那条 31s g++ 编译曲线里,PF 累计 31117 是个无意义的总数——「累计缺页 31117 次」告诉你什么?什么都没告诉你。但 sec 20 的 **+18272** 一眼定位到 cc1plus 加载 libstdc++ 的 demand paging 爆发——「这一秒发生了 18272 次缺页」才是诊断信号。单点采样只能告诉你「现在多少」,趋势采样才能告诉你「**这一秒发生了什么**」。

这条铁律:**任何 ad-hoc profiling 设施的 counter 必须配 delta**,否则曲线是平的、读不出工作负载阶段。注释([diagnostics.cpp:50-52](../../../kernel/mm/diagnostics.cpp#L50))把这个 static 的不变量写得很清楚:

> Delta vs the previous dump so the periodic stats thread's log shows a PF rate, not just a monotonic total. static: dump_memory_stats has no concurrent callers in practice (panic once + the single stats thread).

「no concurrent callers」是 `static` 安全的前提——panic 时调一次 + 单 stats 线程调,两者不会并发,所以 `static last_pf` 不需要锁。这条不变量要记住:如果哪天加了第二个调用者(比如某个 syscall 也调 `dump_memory_stats`),这个 `static` 就 race 了,得加锁或者改成 per-caller state。

## PF 计数器本身:多核下原子是底线

PF 计数器在 [page_fault.cpp:62](../../../kernel/arch/x86_64/page_fault.cpp#L62):

```cpp
// kernel/arch/x86_64/page_fault.cpp:59-75
// Cumulative #PF count for ad-hoc profiling (B1 gcc-compile-stutter).  handle_pf
// runs at IF=0 but multiple CPUs can fault concurrently under -smp 2, so a plain
// ++ would race; an atomic add is cheap and correct.
uint64_t g_pf_count = 0;
}  // namespace

// Total #PF since boot (atomically bumped by handle_pf).  Defined next to the
// counter; declared in fault_diag.hpp so dump_memory_stats can read it without
// dragging in the whole PF handler.
uint64_t pf_count() {
    return __atomic_load_n(&g_pf_count, __ATOMIC_RELAXED);
}

extern "C" {

void handle_pf(InterruptFrame* frame) {
    __atomic_fetch_add(&g_pf_count, 1, __ATOMIC_RELAXED);
    // ... 后续 fault 处理 ...
```

这里有个反直觉点:page fault 在 IF=0 的 fault 上下文里走,看起来天然单线程——一个核在处理 fault 时,这个核的中断是关的。但 CinuxOS 跑 `-smp 2` 时**两核可以同时 fault**——两个核各自跑 cc1、各自 demand-page、各自进 `handle_pf`。plain `++g_pf_count` 在两核并发下会 race 丢计数(经典 read-modify-write 竞争)。所以用 `__atomic_fetch_add(..., __ATOMIC_RELAXED)`——RELAXED 够用,因为只要计数、不要顺序(不需要 #PF 计数和别的内存操作有 happens-before 关系)。

教学:**「中断关闭」不等于「无并发」**。单核下 IF=0 确实串行化了 fault 处理;但多核下,每个核有自己的 IF,你这核关中断不影响别核。多核下原子操作是底线,哪怕看起来「天然单线程」的路径。
