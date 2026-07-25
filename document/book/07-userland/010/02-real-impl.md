---
title: 02 · 真实现:用户态启动各阶段真要用数据的几个
---

# 真实现:用户态启动各阶段真要用数据的几个

这一阵营的共同点是——libc 或用户态程序拿到返回值后,**真要消费里面的字节**。所以这些号都得真给数据,返 ENOSYS 反而会让程序走错。

## `access`:busybox `test -r/-w/-x` 的自主权限裁决

`access(2)` 的用户态需求是 busybox `test` 的 `-r`/`-w`/`-x`、gcc 的 `access` 调用——它们要做一次**「拿自己的身份能不能碰这个文件」**的自主检查(不是 open 它,只是问一声)。`sys_access`(`sys_access.cpp:108`)resolve 路径、查 inode、做一次 `stat` 拿快照、立刻 `inode_unref` 丢掉 inode 引用(`sys_access.cpp:87-92`),之后只对着 `stat` 快照做权限裁决——`access_granted` 是个纯函数(`sys_access.cpp:41`),不碰任何运行时状态。

反直觉点(这章的次明星):**root(uid==0)在 R/W 上全放行,唯独 X_OK 要看 inode 有没有任一执行位**(`imode & 0111`)。这镜像 Linux `generic_permission` 的 root bypass——root 想执行一个文件,文件自己也得至少有一个 x 位。后果很具体:busybox 以 root 跑时,`access(R_OK/W_OK)` 几乎必过(文件不存在才会 `ENOENT`),但 `access(X_OK)` 对一个 `0644` 纯数据文件会真的返 `-EACCES`。这是 root **唯一**会被 `access` 拒的场景,文件头注释第 7-9 行直接点破了这一条:

```
busybox/gcc run as root, so R/W generally succeed; X requires an execute bit --
which is the one denial a root caller can still hit (access(X_OK) on a 0644 file).
```

(`sys_access.cpp:7-9`。root bypass 在 `sys_access.cpp:46-53`,X_OK 看执行位那行是 52。)

诚实边界:`access` 没有 ACL、没有 capabilities,只有 root bypass + 标准 owner/group/other 三元组(`sys_access.cpp:54-58`)。这是 Linux `generic_permission` 的最小子集,够 busybox/gcc 用,真要做多用户系统得扩。

## `getrandom`:glibc 启动的硬需求

glibc 启动链里有一道硬需求——**stack canary、ASLR 种子、fd_set 扰动都要随机源**。如果 `getrandom(2)` 返 ENOSYS 或者不真给字节,带 SSP(StackSmashProtection)编出来的程序会在启动时直接挂掉,或者 canary 全零一捅就破。所以这个号必须真给字节。

`sys_getrandom`(`sys_getrandom.cpp:22`)从内核 PRNG(`KRandom`,xoshiro256\*\* 流)灌字节给用户 buffer。两个反直觉点。

**第一,flags 形参连名字都没给**——签名是 `uint64_t /*flags*/`,完全忽略。注释明说「the kernel PRNG never blocks and is the single random source」(`sys_getrandom.cpp:7-8`)。这是「**ABI 形状对、语义降级**」的典型:号对、参数位置对、真给字节,只是不区分 `GRND_NONBLOCK`/`GRND_RANDOM` 两个源——因为内核只有一个 PRNG。libc 拿到字节就满意,不会因为 flags 被忽略而走错路。

**第二,fill 写的是内核 scratch(`kbuf[256]`),要分块 `copy_to_user` 推过 SMAP 边界**(`sys_getrandom.cpp:30-40`)。任何「内核生成→用户消费」的 syscall 都得过这关——内核不能直接写用户内存(SMAP 拦着),得先写自己的 `kbuf`,再用 `copy_to_user` 把 SMAP 窗口打开推过去。大数据不能一次塞,栈缓冲有上限(这里 256 字节一个 chunk,超过的循环)。

随机源在哪?`KRandom::init`(`random.cpp:61`)在 boot 时跑一次,seed 是四种熵源混合(`random.cpp:64-73` 逐字):

```cpp
uint64_t seed = rdtsc();
seed ^= cinux::drivers::PIT::get_ticks() << 16;
seed ^= reinterpret_cast<uint64_t>(&g_random) >> 4;  // defeat a fixed guess
const bool rdr = has_rdrand();
if (rdr) {
    uint64_t r;
    if (rdrand64(&r)) {
        seed ^= r;
    }
}
```

`rdtsc`(启动周期数)+ `PIT::get_ticks`(启动时序)+ 内核镜像地址(假装 KASLR)+ `rdrand`(CPU 有就掺,helper `has_rdrand()` 查的是 `CPUID.01H:ECX[30]`,`random.cpp:32`)。然后用 splitmix64 把这个 64 位 seed 扩成 256 位**非零** xoshiro 状态——注释明说「an all-zero state would make xoshiro stuck」(`random.cpp:75-76`),全零状态会让 PRNG 卡死输出常数。

诚实边界:`getrandom` 不是 CSPRNG。boot 后状态就固定了(同一个 seed 给出同一条 xoshiro 流),对 ASLR/canary 够用,做密码学密钥材料不合规。源码头注释自己声明了 honest scope——「good-enough entropy for canary/ASLR, not a CSPRNG」。真要做密码学得接硬件 RNG 的连续重采样,这是后续的事。

## `pread64`:不推进 offset 不是靠锁,是靠接口签名

glibc 的动态加载器(`ldso`)精确读 ELF 段和 notes 段要它——**带偏移读,但不挪动 fd 当前的 offset**。`ldso` 可能要先 `pread64` 读 program header,再回头用普通 `read` 顺序读正文,如果 `pread64` 把 offset 推进了,后面的 `read` 就会跳字节。

这章的头号明星反直觉点在这:`sys_pread64` 不推进 `file->offset` **不是靠存旧值恢复、也不是靠锁保护**,是靠接口签名。看 `do_pread64_kernel`(`sys_pread64.cpp:25`):

```cpp
auto read_result =
    file->inode->ops->is_page_cacheable()
        ? cinux::mm::g_page_cache.read_bytes(file->inode, offset, kbuf, count)
        : file->inode->ops->read(file->inode, offset, kbuf, count);
```

`offset` 是**调用者传进来的入参**,从头到尾只用了这一次,压根没写 `file->offset +=`。对照 `do_read_kernel`(`sys_read.cpp:48-58`)才显式 `file->offset_lock_.guard()` + `file->offset += read_result.value()`——`sys_read` 才是「读 + 推进 offset」的两个动作,`sys_pread64` 只有读、没推进。

能这样,是因为 `InodeOps::read` 的签名把 offset 当参数(`inode.hpp:81`):

```cpp
virtual cinux::lib::ErrorOr<int64_t> read(const Inode* inode, uint64_t offset, void* buf,
                                          uint64_t count);
```

offset 是**参数**不是**状态**。`sys_read` 选择「把 `file->offset` 当参数传进去、读完再写回」(于是状态推进了),`sys_pread64` 选择「把调用者给的 offset 当参数传进去、不写回」(于是状态没动)。同一个 `InodeOps::read` 接口、两种用法——这是接口设计层面的干净分离,不是运行时的锁或回滚。

很多学习者第一次读 `pread` 会以为它「内部存旧 offset、读完恢复」——这是从应用层「`fseek`+`fread`+`fseek` 回去」那种实现思路带过来的误解。内核侧根本不需要存,因为它压根没改过。

诚实偏差:`sys_pread64` 对 pipe/pty 等 non-seekable fd 不返 Linux 标准的 `ESPIPE`,而是坍缩成 `EBADF`——因为 Cinux 没把 stdin/console 当 pread 目标(`sys_pread64.cpp:41-43` 明说)。源码注释承认这是偏离:

```
// pread on a non-seekable fd (stdin/console/pipe) is ESPIPE on Linux; we do
// not host those as pread targets, so collapse to EBADF (as never preads fd 0).
```

(`sys_pread64.cpp:41-43`。)

## `prlimit64`:「让 libc 满意」而非「真做资源管控」的标本

glibc 的 malloc 启动会反复探 `RLIMIT_AS`/`RLIMIT_DATA` 估 arena 该开多大(具体次数因 glibc 版本而异,这里不写死)。Cinux 不强制任何资源限制——`brk`/`mmap` 按需长到用户 VA 窗口上限,所以**正确答案就是「所有 limit 都是无限」**。

`sys_prlimit64`(`sys_prlimit64.cpp:33`)实现朴素到近乎寒酸:

```cpp
int64_t sys_prlimit64(uint64_t /*pid*/, uint64_t /*resource*/, uint64_t /*new_rlim_virt*/,
                      uint64_t old_rlim_virt, uint64_t, uint64_t) {
    if (old_rlim_virt != 0) {
        krlimit lim{kRlimInfinite, kRlimInfinite};
        if (!cinux::user::copy_to_user(...)) return -cinux::kEfault;
    }
    return 0;
}
```

`pid`/`resource`/`new_rlim` 三个参数在签名里就被 `/*...*/` 注释掉忽略——**完全没有 enforcement path**。`RLIMIT_NOFILE` 不真限 fd 表、`RLIMIT_STACK` 不限栈、`RLIMIT_CORE`/`AS`/`DATA` 全无限。`old_rlim` 写一对 `RLIM_INFINITY`(就是 `~0ULL`,`sys_prlimit64.cpp:29`)。

为什么不返 `-ENOSYS`?这是个反直觉点:**返 ENOSYS 会让 libc 以为这个 syscall 不存在**——某些 glibc 版本会因此走 fallback 路径(`getrlimit` 老号或者硬编码默认),反而不对。返 0 + 无限 limit 才是「功能存在但策略是无限」的正确表达。所以 prlimit64 是「收下 new_rlim 忽略、返无限 limit 哄过 probe」的策略型——它不是真实现(没 enforcement),也不是纯 stub(不返 ENOSYS),是第三类:**让 libc 满意但不真做**。

这条决策原则贯穿全章:「收下忽略」(`prlimit64`)和「返 ENOSYS」(`sendfile`)是两件完全不同的事——前者等于「支持且成功」,后续走真路径踩空;后者是显式「不支持」,libc 立刻降级。选哪个,看「libc 拿到这个值后会走哪条路」。

## `time` + `gettimeofday`:日志和 timestamp 要时间

这两个号的用户态需求很直白——日志、timestamp、`time()` 库函数。它俩的实现串得很干净,因为都委托给 `do_clock_gettime_kernel(CLOCK_REALTIME)`(`sys_time.cpp:36, 53`)。

`sys_gettimeofday`(`sys_time.cpp:33`)拿 `timespec` 拆成 `timeval{tv_sec, tv_usec = nsec / 1000}`(`sys_time.cpp:42-43`),时区参数 obsolete 忽略(`/*tz_virt*/`)。`sys_time`(`sys_time.cpp:51`)更朴素——返 `tv_sec`,顺手把秒写穿用户指针(若非 0)。

时间源统一出口在 `sys_clock_gettime.cpp:40`:

```cpp
uint64_t monotonic_ns() {
    if (cinux::drivers::g_hpet.available()) {
        return cinux::drivers::g_hpet.monotonic_ns();
    }
    return cinux::drivers::PIT::get_uptime_ms() * kNsPerMs;
}
```

HPET 可用就用、不可用退化 PIT uptime。`CLOCK_REALTIME` = RTC boot epoch + monotonic delta(`sys_clock_gettime.cpp:55-57`)。

这里有个反直觉点(写在诚实边界里也行):你以为 `CLOCK_REALTIME` 每次都重读 RTC,其实 **RTC 只在 boot 时读一次粗秒**,之后全靠 HPET 单调增量精化到纳秒。源码注释管这叫「drift correction」——不重读 RTC(慢、且精度只到秒),用 HPET 持续推进。这个设计反直觉但正确,boot epoch 是定死的、只有 HPET 部分实时读。
