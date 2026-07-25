---
title: 01 · 导引:点亮什么
---

# 导引:点亮什么

> 053 把「调度迁移写花 ctx」这个最显眼的雷扫了,可 SMP 上会写花状态的地方远不止 runqueue。`ext2` 把读过的 inode 缓在一张跨核共享的表里,入口函数 `get_cached_inode` 全程裸奔——两核同时 miss 同一个 ino,一头在 `evict` 把某 slot 删了,另一头手里还攥着指向那个 slot 的 `Inode*` 在用,slot 一被释放或被重填成另一个文件,指针就成了别名 UAF,读到的字节不属于你那个文件。这种错连 ASAN 都看不见(对象还活着,只是内容被换了),是比 panic 难抓十倍的静默别名。
>
> 这一章咱们要做的事,不是「把这一处修了就收工」,而是把 SMP 上「抓竞态」这件事拧成一条红线——它贯穿四个阶段:一是先造一台跨核交错报警器(`race-detect`),盯着「这块共享状态压根没锁」这类设计缺陷;二是拿 `ext2 inode_cache` 当靶子,看报警器真能抓;三是给病灶上真锁、把报警器换成 `lockdep_assert_held` 当回归护栏,顺手清三笔旧债;四是在纵深阶段推导出「同步 TLB shootdown 跑在缺页中断的 IF=0 里会确定性互锁」——正是这个结论逼出了 deferred CoW 设计范式,把跨核 free 从会互锁的中断上下文挪到可阻塞的 drain 内核线程,把 044 章那两本账(`pte_count`/`refcount`)的 `no_free` 变体兑现掉。
>
> 诚实边界先摆前面:三个 `ext2` 盘元数据的 race(`block_buf_` 共享 buffer 互踩、位图分配 RMW 无锁、与之绑定的样式整理)依赖 `KmBuf` RAII 和 `ext2_dirops` 拆分这块地基,划给 080/081;deferred CoW 的**基建与接线**(`handle_cow_fault` 走 `_no_free` + `enqueue_pending_shootdown`、drain 线程已起、`CINUX_TLB_DRAIN` option 默认 ON、0xE1 shootdown IPI 已注册进 IDT)在本机工作树均已落地。WSL2 上 `-smp 2` 靠 KVM 能跑,但 host 的 SMAP/CPUID 透传限制对 race-detect 本身无影响(race-detect 不依赖 SMAP),真机上的 heisenbug 会单独说。

## 这章咱们要点亮什么

1. **race-detect 这台报警器**:它盯的不是时间窗口或数据撕裂,而是「某个共享可变状态本该被自旋锁串行化、实际却没加锁」这一类具体缺陷;机制极简——一个 `RaceWatchpoint` 只记「上一个碰它的 CPU 是谁」,跨 CPU 交错就报警。
2. **opt-in 门控是一条链**:option 开了 ≠ 编译宏传了 ≠ 机制测试真跑了 ≠ 测试真 PASS。断任何一环,日志照样绿、哑火不报错。这条链在本机工作树三环已通(`cmake/options.cmake` 声明 option、`kernel/CMakeLists.txt` foreach 自动传编译宏、§14 文件门按 option 链真实现),lab 会带你亲手开 option 并验证 PASS 亮起。
3. **同一缓存的两层加固**:上一轮结构加固先把 inode cache 从「固定值数组」改成「堆分配 + 分离链 + refcount」治结构性别名 UAF(地址即身份,refcount>0 绝不移动);这一章再叠一把 `inode_cache_lock_` 治并发(两个核同时改这张表)。一个治「谁能在何时被释放」,一个治「谁能同时进来改」,缺一不可。
4. **deferred CoW 的死锁推导**:为什么同步 shootdown 挂在 CoW fault 里会互锁——`Spinlock::guard` 不动 IF,等锁时 IF 保持 0,hole 成立;破局不是「加锁」而是「换执行上下文」,把 shootdown 从 IF=0 的 ISR 搬到 IF=1 的 kthread。
