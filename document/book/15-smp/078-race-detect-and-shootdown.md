---
title: 078 · SMP 竞态——从发现到根治
---

# 078 · SMP 竞态——从发现到根治

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

## 主线一 · race-detect 基建:一台跨核交错报警器

### 它盯的是什么

lockdep 和 race-detect 是两把互补的钳子,分工很明确。`lockdep` 看的是「锁与锁之间的关系」——持锁栈、锁序图、AB-BA 死锁、`schedule()` 跨上下文切换还持锁的断言;`lockdep_assert_held` 防的是「设计了锁、但某条路径忘了拿」这种**已修 race 的回归**。race-detect 看的是另一头——「**根本没有锁**」:`inode_cache_` 这种压根没设计锁、所以也没有任何锁可以 assert 的共享状态,lockdep 管不着,只能靠 race-detect 的跨核交错信号揪出来。两者在同一个头文件里合体:

```cpp
// race_detect.hpp —— 两把钳子同处一屏(已略去 namespace 包裹与头注释)
#ifdef CINUX_LOCKDEP
#    define lockdep_assert_held(lock)                                        \
        do {                                                                 \
            if (!cinux::proc::lockdep_is_held((lock))) {                     \
                cinux::lib::kpanic("lockdep: assert_held failed %p",         \
                                   static_cast<const void*>(lock));          \
            }                                                                \
        } while (0)
#else
#    define lockdep_assert_held(lock) ((void)0)
#endif

#ifdef CINUX_RACE_DETECT
#    define RACE_TOUCH(w) cinux::proc::race_check_access((w))
#else
#    define RACE_TOUCH(w) ((void)0)
#endif
```

（[race_detect.hpp](kernel/proc/race_detect.hpp#L69-L87)。`lockdep_assert_held` 真实宏体是一个带 `kpanic` 的 `do/while(0)`——它是断言不是空操作;宏门控的好处下面讲。）注意「设计了锁但某条路径忘了拿」和「根本没设计锁」是两种不同的病——前者有锁可以 assert,后者连 assert 的对象都没有。

### 机制:一次原子 exchange 拿到「上一个是谁」

看门点本体极简,两个字段:

```cpp
struct RaceWatchpoint {
    const char*       name;       // 报错用
    volatile uint32_t last_cpu;   // 唯一状态:上一个碰它的 CPU
};
```

（[race_detect.hpp](kernel/proc/race_detect.hpp#L46-L49)。初值 `kRaceCpuNone = 0xFFFFFFFF`,意思是「还没人碰过」。）访问点用 `RACE_TOUCH(w)` 宏,内核做的是一次 `__ATOMIC_ACQ_REL` 的 `atomic_exchange_n`——把本核的 cpu id 写进 `last_cpu`,同时拿到「上一个是谁」:

```cpp
bool race_check_access_probe(RaceWatchpoint& w) {
    const uint32_t cpu  = percpu()->cpu_id;
    const uint32_t prev = __atomic_exchange_n(&w.last_cpu, cpu, __ATOMIC_ACQ_REL);
    return (prev != kRaceCpuNone && prev != cpu);
}
```

（[race_detect.cpp](kernel/proc/race_detect.cpp#L17-L21)。）如果 `prev` 既不是 `kRaceCpuNone`(还没人碰过),也不是本核自己,那就说明在上一次访问和这次之间,**另一个 CPU 碰过它且中间没有锁**——这就是跨核交错。`race_check_access` 在此基础上 `backtrace()` + `kpanic("[SMP-RACE] xxx: cpuN touched after cpuM without lock")`,backtrace 从 `RACE_TOUCH` 调用点往上走,正好指到竞态现场([race_detect.cpp](kernel/proc/race_detect.cpp#L23-L34))。

为什么要强调「无锁」?因为看门点报的是**任何**跨 CPU 交错,哪怕两次访问在时间上是串行的。这是刻意的——hobby 内核里每个共享可变状态都**应当**带锁,「两核无锁碰同一状态」本身就是设计缺陷,逼你加锁,而不是靠时序侥幸。加锁之后这个看门点就该拆掉(下面主线三讲)。

### 双入口:probe 不挂、access 挂

两个入口分工:

- `race_check_access`(真凶):检测到交错就 `backtrace()`+`kpanic` 挂内核,适合生产抓真竞态;
- `race_check_access_probe`(探针):只返 `bool` 不挂,机制测试用它。

为什么测试要用 probe?因为测试要验证的是「检测逻辑对不对」,不是「真有一段竞态代码」。用 `access` 会直接把测试套件打飞——测试自己就成了那个「凶手」,还没来得及断言就 panic 了。所以机制测试只 probe、不 touch。

### 海森堡悖论:为什么默认关

opt-in 默认关,理由不只是「每个监控点都插了一次 ACQ_REL 原子 exchange,有开销」。更危险的是它**改了访存时序**——竞态是出了名的 timing-sensitive(海森堡式 bug),插探针可能把要抓的竞态「吓跑」(假阴),也可能造出本来没有的交错(假阳)。所以看到 race-detect 报了,不能假定没探针时也会报;没报,也不能假定没探针时也没事。它是**强信号不是证明**(KCSAN、TSan 同理)。它只适合 debug/测试构建开,生产必须拔。

门控靠三件套,任何一环断了就哑火:

1. `CMake option(CINUX_RACE_DETECT ... OFF)` —— 根 `CMakeLists.txt` 声明开关;
2. `target_compile_definitions(... CINUX_RACE_DETECT)` —— `kernel/CMakeLists.txt` 把编译宏传下去,宏没定义时 `RACE_TOUCH` 展开成 `((void)0)`,访问点处完全不用写 `#ifdef`;
3. **§14 文件门** —— `kernel/proc/CMakeLists.txt` 里 `if(CINUX_RACE_DETECT)` 链 `race_detect.cpp` 真实现,`else` 链 `race_detect_stub.cpp` 空实现(只为满足链接器符号):

```cpp
// race_detect_stub.cpp —— OFF 时空实现
bool race_check_access_probe(RaceWatchpoint& /*w*/) { return false; }
void race_check_access(RaceWatchpoint& /*w*/)       {}
```

（[race_detect_stub.cpp](kernel/proc/race_detect_stub.cpp#L14-L22)。）这套设计的关键是**「生产关、测试开」零侵入**——`RACE_TOUCH` 和 `lockdep_assert_held` 都是宏,编译宏没定义时直接 `((void)0)`,访问点处一行 `#ifdef` 都不用写。

## 主线二 · 拿 inode_cache 当靶子:验证报警器真能抓

### inode_cache 是完美的靶子

工具建好不能空跑——要拿一个**确定有 race 的目标**喂给它,看它报不报。`ext2` 的 `inode_cache_` 就是完美的靶子。它把读过的 inode 缓在内存里(分离链 hash 表,`ino % 512` 个桶),入口 `get_cached_inode(ino)` 做三件事:walk(沿桶链找命中)/ evict(miss 且缓存满时找 `refcount==0` 的对象腾位置)/ insert(new 一个 `Ext2CachedInode`、`read_disk_inode` 读盘、populate 成 VFS Inode、挂桶头)。

这张表是 `Ext2` 实例成员、跨 CPU 共享。但在加锁前三步全裸奔——SMP 下两核可以同时 miss 同一个 ino:

- 核 A 走到 `evict` 把某个 slot 删了;
- 核 B 此刻正拿着指向那个 slot 的 `Inode*` 在用(fd 或 VMA 在引用);
- slot 被释放、或被重填成另一个 ino;
- 核 B 手里的指针就成了别名 UAF,读到「另一个文件的字节」。

这类错 ASAN 都看不见——对象还活着,只是内容被换了,是**静默别名**。这正是前面讲过的那类「比 panic 难抓十倍」的 bug。

### 设计意图 vs 当前工作树

讲「race-detect 作为工具能抓」,用 `kernel/test/main_test.cpp` 里的机制自测当例子最稳妥——它证明了检测逻辑本身端到端通:

```cpp
#ifdef CINUX_RACE_DETECT
static cinux::proc::RaceWatchpoint g_race_test_wp =
    RACE_WATCHPOINT_INIT("test.race_detect");
#endif
```

（[main_test.cpp](kernel/test/main_test.cpp#L910-L917)。机制测试的 watchpoint。）测试的意图是:AP 先碰一次这个 watchpoint,BSP 再 probe,probe 拿到的 `prev` 是 AP 的 cpu id、不等于 BSP 自己,返 `true` = 检测到跨核交错,打出那一行专属的 PASS,FAIL 则把整个 suite 拖红:

```cpp
#ifdef CINUX_RACE_DETECT
    if (ok && ap_count > 0) {
        const bool race = cinux::proc::race_check_access_probe(g_race_test_wp);
        cinux::lib::kprintf("[F-DYN-COV] race-detect test: %s\n",
                            race ? "PASS (detected cross-CPU)" : "FAIL (no cross-CPU seen)");
        if (!race) {
            ok = false;   // FAIL 不是只打印——它会拖垮整 suite,这才是「不是哑火」的证据
        }
    }
#endif
```

（[main_test.cpp](kernel/test/main_test.cpp#L1048-L1064)。用 probe 不用 `RACE_TOUCH`——机制测试不能挂内核;末尾 `if (!race) { ok = false; }` 让 FAIL 真的拖红整个 suite,这正是这一行测试「没被哑火」的硬证据。)

⚠️ **这里有个细节值得说清楚**:`main_test.cpp` 注释写着「AP touches it in `ap_test_selfcheck` before writing magic」,翻 `ap_test_selfcheck` 函数体([main_test.cpp](kernel/test/main_test.cpp#L927-L960))确实如此——L940 一行 `#ifdef CINUX_RACE_DETECT race_check_access_probe(g_race_test_wp); #endif` 把 AP 侧的 touch 补上了。BSP 的 probe 拿到的 `prev` 是 AP 的 cpu id、不等于自己,返 `true` = 检测到跨核交错,打出 PASS。也就是说:只要三件套(option + 编译宏 + §14 文件门)齐了,这条机制自测端到端通、能真报 PASS。lab 会带你亲手补全三件套并验证这行 PASS 真的会亮。

至于 `ext2` 路径本身——CinuxOS 上游(提交 `bf2d2d7`)确实在 `get_cached_inode` 入口放过 `RACE_TOUCH(g_inode_cache_wp)`,GUI `-smp 2` 跑 gcc 立刻抓到凶手栈 `get_cached_inode ← execve /bin/sh`,证明检测器对真实代码路径有效。但 Book 回迁时**只落地了「锁 + `lockdep_assert_held` + old-style cast 修复」三样**,那个 `RACE_TOUCH` 靶子没回迁(Book 树 `grep kernel/fs/` 零命中)。所以教程里讲「race-detect 作为工具能抓」,用机制自测当例子;`ext2` 路径讲 `lockdep_assert_held` 当回归护栏——两个工具互补,A 抓「有锁忘持」、B 抓「根本没锁」。

### 抓 → 修 → 换护栏的标准闭环

这是验证期最该记住的范式:

1. **加锁前**用 `RACE_TOUCH` 抓「根本没锁」——检测器报警 = 证实有竞态;
2. **加锁后**把 `RACE_TOUCH` 删掉换成 `lockdep_assert_held`——因为加锁后两核串行访问**也是**跨 CPU 交错,race-detect 会误报(它不区分「有锁串行」和「无锁交错」);这时换成 assert,防的是「未来有人重构把 guard 调用删了」这种回归。

不换会怎样?加锁了还留着 `RACE_TOUCH`,LOCKDEP 构建下两个核正常串行访问也会被 race-detect 当成 race 打 panic——报警器反而成了误报源。所以「抓完就换」是必须的,不是可选的。

## 主线三 · 病灶上锁:inode_cache 套大锁,顺手清三笔旧债

### 主线:整段 walk/evict/insert 套进一把自旋锁

修法很直白——给 `inode_cache_` 配一把 `inode_cache_lock_`,在 `get_cached_inode` 入口拿锁 + 断言:

```cpp
Inode* Ext2::get_cached_inode(uint32_t ino) {
    // B3 inode cache race: inode_cache_ is shared across CPUs.  Serialize the
    // walk/evict/insert under inode_cache_lock_.  Held across read_disk_inode
    // (disk I/O) -- cache miss is rare and slow anyway; dropping the lock for
    // I/O would need a TOCTOU recheck.  lockdep_assert_held is the regression
    // guard if a future refactor drops the guard.
    auto g = inode_cache_lock_.guard();
    lockdep_assert_held(&inode_cache_lock_);
    if (ino == 0) { return nullptr; }
    // ... walk / evict / insert 全在锁内 ...
}
```

（[ext2_inode.cpp](../../../libs/ext2/ext2_inode.cpp#L93-L103)。注释把设计取舍讲透了。）锁成员声明挨在 cache 表旁边:

```cpp
Ext2CachedInode* inode_cache_[EXT2_INODE_CACHE_SIZE]{};
uint32_t inode_cache_count_{0};
mutable cinux::proc::Spinlock inode_cache_lock_;  ///< SMP: serialize cache walks/evicts
```

（[ext2.hpp](../../../libs/ext2/ext2.hpp#L491-L495)。结构(state)和并发(lock)两层加固同处一屏。)

#### 关键取舍:持锁跨 read_disk_inode 的盘 I/O

教科书会警告「别持自旋锁做 I/O」——持锁期间别的核要访问这张表就得 spin,盘 I/O 又慢。但这里合理,理由是 cache miss 本就是**稀有且慢**的路径:如果为了放锁去做「先占坑、放锁读盘、再加锁检查有没有别人插进来」的 TOCTOU 重检,会引入双倍复杂度(占坑标记、重检逻辑、谁负责读盘),收益不值。所以这里选择**正确性优先、锁覆盖全程**。这是 hobby 内核的典型取舍——先把正确性做扎实,性能等真成了瓶颈再说。

#### 两层护栏

加锁前用 race-detect 抓「根本没锁」;加锁后换成 `lockdep_assert_held` 防回归——万一未来有人重构把 `guard()` 那行删了,LOCKDEP 构建立刻 kpanic,bug 在测试阶段就炸,不会溜到生产。这是「同一缓存的两种检测器分工」的完整闭环。

#### 与结构层加固的关系:治结构 vs 治并发

`inode_cache_lock_` 不是第一层加固。在它之前,上一轮地基重做先把 inode cache 从「固定值数组」改成「堆分配 + 分离链 + refcount」:

```cpp
struct Ext2CachedInode {
    Ext2Inode        disk_inode;    ///< Copy of the on-disk inode
    Inode            vfs_inode;     ///< VFS-facing inode
    uint32_t         ino{0};        ///< Inode number …
    bool             stale{false};  ///< Disk inode changed under a live ref …
    Ext2CachedInode* hash_next{nullptr};  ///< Separate-chaining link …
    // …
};
```

（[ext2_types.hpp](../../../libs/ext2/ext2_types.hpp#L334-L340)。）那层治的是**结构性别名 UAF**——「slot 被驱逐重填导致活指针失效」。对象的地址即身份,只要 `refcount>0` 就绝不移动/重填,驱逐只挑 `refcount==0` 的:

```cpp
// evict:缓存满时只驱逐 refcount==0 的;全活则失败不腐蚀
if ((*pp)->vfs_inode.refcount == 0) { victim_prev = pp; break; }
// ...
if (victim_prev == nullptr) { return nullptr; }  // 全在用,失败也不重填活对象
```

（[ext2_inode.cpp](../../../libs/ext2/ext2_inode.cpp#L133-L150)。）但结构层加固只保证「单个 CPU 内、单线程语义下指针稳定」,没管「两个核同时进来改这张表」——那是这一章的活。**一个治结构(谁能在何时被释放),一个治并发(谁能同时进来改),缺一不可**。光有 refcount 不加锁,两核照样能同时 `new` + `read_disk_inode` + `insert`,重复读盘、重复挂桶;光有锁不保证地址即身份,驱逐重填照样让活指针失效。

### 顺手 rider:三个正确性债,两种结局

主线之外,这一轮顺手收了三笔正确性债。要诚实分开讲——两个修成了,一个没修成。

#### Rider ① NVMe io_submit —— 已落地

SMP 下两核同时往同一个 IO 队列塞命令,共享的 SQ tail / CQ head / phase 会被互相踩。修法是把「塞命令 + 等 completion」串进一把 `io_lock_` 的临界区:

```cpp
ErrorOr<uint16_t> NvmeController::io_submit(const NvmeCmd& cmd) {
    // SMP: serialize SQ enqueue + CQ poll -- io_sq_tail_/io_cq_head_/io_cq_phase_
    // are shared; two CPUs submitting at once clobber each other's sq slot and
    // mis-read completions (status=0x4080 on a legal LBA).
    io_lock_.acquire();
    // ... SQ enqueue + doorbell + 整个 CQ poll 循环 ...(末尾 io_lock_.release();)
}
```

（[nvme_io.cpp](kernel/drivers/nvme/nvme_io.cpp#L20-L103)。`io_submit` 在 SMP 重构时拆出独立文件,锁用手动 `acquire()`/`release()` 包整段而非 RAII guard——因为循环中途有 yield 重入点。注释写明 race 表现:合法 LBA 读到 `status=0x4080`。）对照一下:`admin_submit` 无锁——它在 init 期单线程跑,不存在并发。

#### Rider ② ELF 加载校验 —— 已落地

损坏或恶意 ELF 头能让地址算术溢出回绕成小地址、或逼内核 alloc 几 MB 的 phdr 表。两道护栏:

```cpp
// validate_load_segment:三处 __builtin_add_overflow 截住地址回绕
if (__builtin_add_overflow(base, phdr.p_vaddr, &seg_vaddr) ||
    __builtin_add_overflow(seg_vaddr, phdr.p_memsz, &seg_memsz_end) ||
    __builtin_add_overflow(phdr.p_offset, phdr.p_filesz, &file_off_end)) {
    return ExecveResult::BadElfHeaders;
}
constexpr uint64_t kUserVaTop = 0x800000000000ULL;   // canonical user VA 上界兜底
if (seg_vaddr >= kUserVaTop || seg_memsz_end > kUserVaTop) {
    return ExecveResult::BadElfHeaders;
}
```

（[elf_load.cpp](kernel/proc/elf_load.cpp#L41-L59)。GCC 没有 unsigned overflow 的 sanitize,只能靠 `__builtin_*_overflow`。）配套给 `e_phnum` 加上限,挡住损坏 ELF 逼内核 alloc + read ~3.6MB phdr 表:

```cpp
constexpr uint16_t kMaxPhnum = 256;   // real ELFs <30,256 是工程经验值不是规范值
if (ehdr->e_phnum > kMaxPhnum) { return ElfValidateResult::BadPhnum; }
```

（[elf_types.cpp](kernel/proc/elf_types.cpp#L63-L72)。）

#### Rider ③ VFS offset_lock —— 已落地(分流锁)

这一笔是 schedule-while-held 这类 LOCKDEP 头号死锁模式的根治。病根曾经是:`do_read_kernel` / `do_write_kernel` 无条件持 `file->offset_lock_` 再调 `read` / `write`,而非 cacheable 路径(pipe / pty)的 `read` 会 `schedule_blocked` 让出 CPU——持着自旋锁去 schedule。

修法是用 `is_page_cacheable()` 把锁分流:只有 disk 路径(走 PageCache + demand page + NVMe poll,不阻塞)才持 `offset_lock_` 并更新 offset;pipe / pty 这类会 `schedule_blocked` 的流式路径不持锁、unlocked 更新 offset:

```cpp
int64_t do_read_kernel(int fd, void* kbuf, uint64_t count) {
    // offset_lock_ guards file->offset (seek position).  Only disk-backed
    // (page_cacheable) files use offset; their read path (PageCache + demand
    // page + NVMe poll) does not block on schedule.  Pipes/pty are streams --
    // their read() calls schedule_blocked, so holding offset_lock_ across it
    // would deadlock (LOCKDEP: schedule-while-held).
    if (file->inode->ops->is_page_cacheable()) {
        auto g           = file->offset_lock_.guard();   // disk 路径才持锁
        auto read_result = cinux::mm::g_page_cache.read_bytes(...);
        // ... 更新 file->offset ...
    } else {
        // pipe/pty:unlocked 更新 offset,read() 内部可安全 schedule_blocked
    }
}
```

（[sys_read.cpp](kernel/syscall/sys_read.cpp#L48-L57)。`sys_write.cpp:53` 同样已分流。）这条修法对应 CinuxOS 上游(提交 `f40bed1`),已回迁到 Book 工作树。

配套对比点很值得记住——`sys_lseek` 持 `offset_lock_` 改 offset 是对的([sys_lseek.cpp](kernel/syscall/sys_lseek.cpp#L34-L35)):它做的是纯算术、不阻塞,不触发 schedule-while-held;NVMe `io_lock_` 跨 busy-wait poll 也是安全的(poll 不让出 CPU)——错的是「持着它去 `schedule_blocked`」,而分流锁正是把这一刀切干净。

## 主线四 · 纵深期:IPI shootdown 与 deferred CoW 范式

这一阶段把视野从「单张共享表的锁」抬到「跨核 TLB 一致性」——SMP 上一个更难的问题。

### 病根:TLB 是每核私有的,页表是共享的

单核时代,改完页表(unmap / 改权限 / CoW 换页)本核一条 `invlpg`(`flush_tlb`)刷掉这条 TLB 映射就收工,别的核不存在,不存在 stale 缓存。CoW 换页时本地刷完直接 `pte_count_dec_and_test` 把旧页 free,天经地义。

SMP 时代不一样了:本核刷了没用——**别的核的 TLB 里那条旧映射还活着**。核 A 把某虚拟地址的 PTE 改掉、把旧物理页还给 buddy 之后,核 B 的 TLB 还缓存着旧映射,B 继续用旧物理地址读写——读到复用后的别人的数据、写错页(use-after-free)。所以释放旧页前,必须先**广播一个 IPI 给所有其他核,等它们都 `invlpg` 完、ack 回来**,才能 free。

### IPI shootdown 基建:广播 + ack 计数

机制本身很直白,是一次「广播 + ack 计数」的同步握手。发送方([tlb.cpp](kernel/arch/x86_64/tlb.cpp#L34-L58)):

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

IPI 向量挑了 `0xE1`,紧挨 reschedule `0xE0`,刻意避开 PIC IRQ 段(`0x20-0x2F`)、spurious(`0xFF`)、sigreturn(`0x80`)([smp.hpp](kernel/arch/x86_64/smp.hpp#L15-L22))。发送用 Local APIC ICR 的「all-excluding-self」简写(`bits[19:18]=11`),一口气发给所有其他核。

接收端([tlb.cpp](kernel/arch/x86_64/tlb.cpp#L60-L67)):

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

（[pmm.cpp](kernel/mm/pmm.cpp#L279-L289) 和 [pmm.cpp](kernel/mm/pmm.cpp#L327-L350)。这就是 044 章那两本账的 `no_free` 变体——所有权账面归零,实物留着等 drain 兑现。）

2. 把 `{old_phys, vaddr}` 塞进一条 pending 链表 + 给信号量 `post` 一下([tlb.cpp](kernel/arch/x86_64/tlb.cpp#L85-L113)):

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

（[tlb_drain.cpp](kernel/arch/x86_64/tlb_drain.cpp#L36-L47)。deferred 的兑现端。)

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

## 范围与边界(诚实说)

### 三个 deferred commit:依赖 ext2 重构地基,划给 080/081

三个 `ext2` 盘元数据 race 没在这一章修,因为它们都依赖 `KmBuf` RAII 和 `ext2_dirops` 拆分这块地基:

- **`block_buf_[4096]` 共享 buffer 无锁 race** —— 两 CPU 同时 `read_block` 互相覆盖 `block_buf_`,`resolve_disk_block_` 拿到别人的 indirect 数据 → wild block 号 → NVMe 读超范围 failed → demand page 失败 → segfault。修法是堆 `KmBuf` RAII 替共享 buffer(当前 `ext2.hpp` 里仍是裸 `uint8_t block_buf_[4096]`,无 `KmBuf`);
- **盘位图 alloc/free RMW 无锁** —— 两 CPU 同时 alloc 都读同 bit free → 都 mark+write → 同块分给两文件、一覆盖另一。修法是加 `block_alloc_lock_` 串行位图 RMW;
- **样式整理** —— 与 `ext2_dirops` 拆分绑定。

这几笔债还有一层「检测器盲区」的教学价值:`RACE_TOUCH` 能抓 `inode_cache_`(单一访问点),但 `block_buf_` 是被几十处 `read_block` 共享的 scratch,`RACE_TOUCH` 标不过来——这类「共享 scratch 互踩」要等 host TSAN(host-build 直接观察内存访问)才能秒级定位,是 081 的内容。本章只作检测器互补的对照引用,不展开。

### deferred-CoW 基建与接线

整条 deferred-CoW 修复在当前 Book 工作树已端到端接通,逐条交代:

- **`handle_cow_fault` 走 deferred 路径** —— `process_new.cpp` 的 `handle_cow_fault` 已调 `pte_count_dec_and_test_no_free(old_phys)`,命中(计数归零)后再 `enqueue_pending_shootdown(old_phys, fault_vaddr)`,**不再立即 free**([process_new.cpp](kernel/proc/process_new.cpp#L114-L126),注释 L119 自承「B3 defect C: defer the free」);
- **`enqueue_pending_shootdown` 已有调用方** —— 上一条就是它的调用点;
- **`CINUX_TLB_DRAIN` 这个 CMake option 已声明** —— `cmake/options.cmake` `option(CINUX_TLB_DRAIN "Spawn the TLB shootdown drain kthread (deferred CoW free)" ON)`,默认 ON;`kernel/arch/CMakeLists.txt` 的 `if(CINUX_TLB_DRAIN)` 据此决定链 `tlb_drain.cpp` 真实现还是 `tlb_drain_stub.cpp` 空实现;
- **`start_tlb_drain_thread()` 已有调用方** —— `proc/init.cpp` 在初始化阶段调用它起 drain kthread(init.cpp L19 include、L165 调用);
- **`shootdown_ipi_stub`(0xE1)已注册进 IDT** —— `irq_init()` 在 reschedule 0xE0 之后 `set_handler(kShootdownIpiVector, shootdown_ipi_stub, ...)` 注册 0xE1([irq_handlers.cpp](kernel/arch/x86_64/irq_handlers.cpp#L172-L177)),`shootdown_ipi_stub` 声明在 [irq_handlers.cpp:67](kernel/arch/x86_64/irq_handlers.cpp#L67),`interrupts.S` 的 `ISR_IRQ shootdown_ipi_stub, shootdown_ipi_handler, 0` 定义在 [interrupts.S:455](kernel/arch/x86_64/interrupts.S#L455)。

连带机制测试也端到端跑通:机制测试里的 `tlb_shootdown_page(0xDEADB000)`([main_test.cpp](kernel/test/main_test.cpp#L1044-L1048))在 `-smp 2` 下会真发 0xE1 IPI 给 AP、AP ack 回来、BSP 的 spin 等到 acks==0 退出,打出 `[F-VERIFY] shootdown IPI test: PASS (all APs acked)`——0xE1 收发通路在当前工作树已可验证。

### race-detect 门控链在本机工作树的状态

主线一讲过 opt-in 是一条链,本机工作树三环都已接通:

- **`option(CINUX_RACE_DETECT ...)` 已声明** —— [cmake/options.cmake](../../../cmake/options.cmake#L63) `option(CINUX_RACE_DETECT "Enable SMP data-race watchpoint detector (debug)" OFF)`,默认 OFF(opt-in);
- **编译宏通过 foreach 自动传** —— `cmake/options.cmake` 的 `CINUX_COMPILE_DEF_OPTS` 列表含 `RACE_DETECT`([cmake/options.cmake:148](../../../cmake/options.cmake#L148)),`kernel/CMakeLists.txt` 的 `foreach(_opt IN LISTS CINUX_COMPILE_DEF_OPTS)`([kernel/CMakeLists.txt#L130-L135](../../../kernel/CMakeLists.txt#L130-L135))把它自动 map 成 `target_compile_definitions`,加新开关无需改 kernel/CMakeLists.txt;
- **AP 侧机制测试 touch 已落地** —— 上面主线二讲过,`ap_test_selfcheck` 已有 `race_check_access_probe(g_race_test_wp)`。

所以 `-DCINUX_RACE_DETECT=ON -DCINUX_LOCKDEP=ON` 之后,三件套齐、机制自测端到端通、能真报 PASS。lab 会带你亲手开这两个 option 并验证 PASS 亮起。

### VFS offset_lock 分流锁

主线三 rider ③ 已详述:CinuxOS 上游修的 `is_page_cacheable` 分流锁已回迁到 Book 工作树,`sys_read.cpp:48` / `sys_write.cpp:53` 都按 cacheable 与否分流持锁,schedule-while-held 病灶态已根治。

### WSL2 上的验证边界

本机 WSL2 + KVM 能跑 `-smp 2`(qemu-system-x86_64 11.0.2 在 `/usr/sbin/`、`/dev/kvm` 存在且 `crw-rw-rw-`),`run-kernel-test-smp` target 在 `cmake/qemu.cmake` 里定义(QEMU `-smp 2` + test kernel + 自动退出)。race-detect 本身不依赖 SMAP/CPUID 透传,所以 WSL2 的 SMAP 限制对 race-detect 验证无影响。但真机上的 heisenbug(`-smp 2` 偶发踩时序)是环境相关的——race-detect 抓的是「无锁交错」这类**结构性**缺陷,不是时序抖动;真机 heisenbug 要靠 race-detect 开着多跑、看它报不报,单次没报不能下结论(海森堡悖论)。

## 收尾:四块地基拧成一根绳

回看这一章的四个阶段,race-detect 是那根红线:发现期用它证实「`inode_cache_` 根本没锁」;验证期拿它当靶子证明检测器真能抓;根除期病灶上锁后它让位给 `lockdep_assert_held` 做回归护栏;纵深期推导出的「IF=0 不能 spin 等跨核 ack」逼出了 deferred CoW——这条推导的兑现端(`_no_free` + drain kthread)正是 044 章那两本账(`pte_count` / `refcount`)的延展。

四个阶段不是孤立的修法清单,是同一件事的四步:**把 SMP 上「靠人 audit + 靠崩发现」换成「靠机制报警 + 靠结构保证」**。报警器抓结构性缺陷(无锁),结构保证消除时序侥幸(deferred 把「可能互锁」降到「结构上不可能」)。这根绳从 044 的两本账起头,经过 078 的 race-detect 和 deferred CoW,绳结越收越紧——下一章(080/081)会接着把 `ext2` 盘元数据的 race(`block_buf_` / 位图 RMW)和 host TSAN 那套「共享 scratch 互踩」的检测能力补上。

> 诚实边界再压一句:race-detect 那条机制自测(`[F-DYN-COV] race-detect test:`)当前是「测试在跑、断言在执行」的状态——它的检测逻辑端到端通(见主线二的 exchange + 比较),三件套(option + 编译宏 foreach + §14 文件门)齐了、AP 侧 touch 也已落地,`-DCINUX_RACE_DETECT=ON -DCINUX_LOCKDEP=ON` 下 `-smp 2` 跑会真报 PASS。shootdown IPI 那段 0xE1 已注册进 IDT、机制测试已端到端跑通打出 `[F-VERIFY] shootdown IPI test: PASS`;deferred-CoW 的接线(`_no_free` + `enqueue_pending_shootdown` + drain kthread + `CINUX_TLB_DRAIN` option)和 rider ③ 分流锁都已落地——这些「已接通」的事实按上面边界逐条交代,当前树就是「已生效」的现场。
