---
title: 02 · 主线一 · race-detect 基建:一台跨核交错报警器
---

# 主线一 · race-detect 基建:一台跨核交错报警器

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

（[race_detect.hpp](../../../kernel/proc/race_detect.hpp#L69-L87)。`lockdep_assert_held` 真实宏体是一个带 `kpanic` 的 `do/while(0)`——它是断言不是空操作;宏门控的好处下面讲。）注意「设计了锁但某条路径忘了拿」和「根本没设计锁」是两种不同的病——前者有锁可以 assert,后者连 assert 的对象都没有。

### 机制:一次原子 exchange 拿到「上一个是谁」

看门点本体极简,两个字段:

```cpp
struct RaceWatchpoint {
    const char*       name;       // 报错用
    volatile uint32_t last_cpu;   // 唯一状态:上一个碰它的 CPU
};
```

（[race_detect.hpp](../../../kernel/proc/race_detect.hpp#L46-L49)。初值 `kRaceCpuNone = 0xFFFFFFFF`,意思是「还没人碰过」。）访问点用 `RACE_TOUCH(w)` 宏,内核做的是一次 `__ATOMIC_ACQ_REL` 的 `atomic_exchange_n`——把本核的 cpu id 写进 `last_cpu`,同时拿到「上一个是谁」:

```cpp
bool race_check_access_probe(RaceWatchpoint& w) {
    const uint32_t cpu  = percpu()->cpu_id;
    const uint32_t prev = __atomic_exchange_n(&w.last_cpu, cpu, __ATOMIC_ACQ_REL);
    return (prev != kRaceCpuNone && prev != cpu);
}
```

（[race_detect.cpp](../../../kernel/proc/race_detect.cpp#L17-L21)。）如果 `prev` 既不是 `kRaceCpuNone`(还没人碰过),也不是本核自己,那就说明在上一次访问和这次之间,**另一个 CPU 碰过它且中间没有锁**——这就是跨核交错。`race_check_access` 在此基础上 `backtrace()` + `kpanic("[SMP-RACE] xxx: cpuN touched after cpuM without lock")`,backtrace 从 `RACE_TOUCH` 调用点往上走,正好指到竞态现场([race_detect.cpp](../../../kernel/proc/race_detect.cpp#L23-L34))。

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

（[race_detect_stub.cpp](../../../kernel/proc/race_detect_stub.cpp#L14-L22)。）这套设计的关键是**「生产关、测试开」零侵入**——`RACE_TOUCH` 和 `lockdep_assert_held` 都是宏,编译宏没定义时直接 `((void)0)`,访问点处一行 `#ifdef` 都不用写。
