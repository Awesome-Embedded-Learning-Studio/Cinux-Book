---
title: 086 · Linux ABI 拼图:让 glibc/musl/busybox 跑起来,以及「stub 返 ENOSYS 才是对的」这件反直觉事
---

# 086 · Linux ABI 拼图:让 glibc/musl/busybox 跑起来,以及「stub 返 ENOSYS 才是对的」这件反直觉事

> 059 让 musl 静态 hello 跑通,073 让静态 busybox 14 个 applet 跑通——这两章把「能让一个真程序起来」的 ABI 地基铺好了:Linux x86_64 的 syscall 号表、负 errno 的返回约定、铺满 auxv 的初始栈、busybox 试金石和它用到的 `getdents64`/`chmod`/`dup`/`fcntl`/`nanosleep`/`socket` 那一大坨。这一章接的不是另一批真程序,是另一批**契约**:让 glibc 和更激进的 musl 程序在启动和运行各阶段不被一个缺掉的 ABI 卡住——内核要么真给数据、要么诚实地返 `-ENOSYS` 让 libc 优雅降级。这一章一共讲十**四**个 Linux ABI 号 + 一个 Cinux 专有的偏离(`cinux_exit`),其中九个真实现、五个是 stub,但都不是「清单」,是 glibc 启动会挨个探过去的一条 ABI 谱。
>
> A 档:教程即验证。本章核实的是源码侧事实——这十五个号都注册进了 dispatch 表、号都和 Linux x86_64 对齐(只有 `cinux_exit` 占了 Linux `fadvise64` 的号,文档化偏离)、handler 逻辑和源码注释里声明的语义一致、dispatch 兜底返的是 `-kEnosys` 不是裸 `-1`。诚实的边界先放在台面上:九个真实现(`access`/`getrandom`/`pread64`/`prlimit64`/`time`/`gettimeofday`/`tkill`/`setitimer`/`sched_getaffinity`)、五个是返 `-ENOSYS` 或返 0 哄过的探测 stub(`getcpu`/`rseq`/`clone3`/`sendfile`/`set_robust_list`),外加一个 Cinux 专有的 `cinux_exit`。这不是残缺,是务实——内核**判断一个号该真实现还是 stub 的唯一依据,是 libc 拿到这个返回值后行为对不对**,不是「这个 syscall 容不容易实现」。这章就是把这条判断原则讲成可读的东西。

## 这章咱们要点亮什么

1. **判断一个号该怎么应答,看 libc 拿到返回值后的行为**——`getcpu`/`rseq`/`clone3`/`sendfile` 返 `-ENOSYS` 是对的,因为 libc 收到 ENOSYS 会走降级路径;`set_robust_list` 反而返 0 是对的,因为它是个「被满足的探测」,返 ENOSYS 会让 libc 把整条 robust futex 路径判废。同一个文件里,四个返 ENOSYS、一个返 0,差别就是这条原则。
2. **`-ENOSYS` 是正面信号,不是 bug**——dispatch 兜底对未注册号就返 `-kEnosys`(38),因为裸 `-1` 会被 musl/glibc 当成 errno=EPERM(1),libc 以为「syscall 存在但被权限拒」走错路径直接报错;`-ENOSYS` 才是「不存在,请降级」。
3. **`pread64` 不推进 offset 不是靠锁保护,是靠接口签名**——`InodeOps::read(inode, offset, buf, count)` 的 offset 是**入参**,`do_pread64_kernel` 全程只用调用者传的 offset,压根没碰 `file->offset`。对照 `sys_read` 才显式 `file->offset += n`。很多学习者以为 pread 要存旧 offset 读完恢复,其实 offset 根本没动过。
4. **`cinux_exit` 跟 `sys_exit` 是两件完全不同量级的事**——前者 `io_outl(0xf4, code)` 让整个 QEMU 进程退出(暴露给 ring3 的 CI gate),后者把当前 task 标 Zombie、唤醒父进程 reap(进程卷的事)。混淆了会把 CI gate 写成「程序退出就 PASS」。

## 这批 syscall 是 059/073 立的 ABI 基线的延续

先摆清楚边界——这是反清单铁律,这章不重讲 059/073。

- **059 立了 ABI 基线**:Linux x86_64 syscall 号表 + 负 errno 返回约定 + 铺满 auxv 的初始栈(让 musl 启动能读 `AT_RANDOM`/`AT_PHDR`/...)+ musl 静态 hello 端到端跑通。
- **073 立了 busybox 试金石 + 它用到的那批 syscall**:静态 busybox 1.36(musl 编)14 applet 真跑出真输出,顺带补全 `getdents64`/`chmod`/`chown`/`link`/`rename`/`utimensat`/`dup`/`fcntl`/`nanosleep`/`sysinfo`/`getrusage` + socket ABI。

这一章补的是**另一批**——是 glibc(和更激进的 musl 程序)启动会挨个探过去、059/073 还没碰过的那十几个号。它们不在 073 的 busybox 14 applet 主路径上(busybox 073 那批主要走 `getdents64`/`chmod`/`dup`...),而是在 glibc 启动和 busybox 一些更靠后的 applet(ping/nproc/job control)上。

号都在 `kernel/syscall/syscall_nums.hpp:132-141`(下面是源码逐字引用,保留原注释):

```cpp
SYS_sendfile        = 40,   ///< sendfile (stub -ENOSYS; cp falls back to read+write)
SYS_gettimeofday    = 96,   ///< wall-clock time (CLOCK_REALTIME; same source as clock_gettime)
SYS_set_robust_list = 273,  ///< robust-futex probe (stub 0; no real robust cleanup yet)
SYS_prlimit64       = 302,  ///< resource-limit probe (stub; reports RLIM_INFINITY)
SYS_getcpu          = 309,  ///< getcpu (stub -ENOSYS; glibc falls back from per-CPU hint)
SYS_getrandom       = 318,  ///< random bytes (KRandom PRNG)
SYS_rseq            = 334,  ///< restartable-sequence probe (stub -ENOSYS)
SYS_clone3          = 435,  ///< clone3 probe (stub -ENOSYS; libc falls back to clone)
SYS_time            = 201,  ///< time in seconds (CLOCK_REALTIME)
SYS_sched_getaffinity = 204,  ///< CPU affinity mask (busybox nproc / glibc probe)
```

(源码里这块上方挂着一条开发注释「gcc/g++ self-host」——那是源码真貌,咱们原样保留,正文叙述里不再用这批号。)

加上更早就在表里、这一章顺手归类的 `access`(21)/`pread64`(17)/`setitimer`(38)/`tkill`(200),一共十**四**个 Linux ABI 号。再单独算一个 Cinux 专有的 `cinux_exit`(221),它是第**十五**个,不在这条 Linux ABI 谱里(后面单独讲为什么)。本章后面按**它解决什么用户态需求**分阵营讲,不按 syscall 字母表——那才是 changelog,不是教程。

## 真实现:用户态启动各阶段真要用数据的几个

这一阵营的共同点是——libc 或用户态程序拿到返回值后,**真要消费里面的字节**。所以这些号都得真给数据,返 ENOSYS 反而会让程序走错。

### `access`:busybox `test -r/-w/-x` 的自主权限裁决

`access(2)` 的用户态需求是 busybox `test` 的 `-r`/`-w`/`-x`、gcc 的 `access` 调用——它们要做一次**「拿自己的身份能不能碰这个文件」**的自主检查(不是 open 它,只是问一声)。`sys_access`(`sys_access.cpp:108`)resolve 路径、查 inode、做一次 `stat` 拿快照、立刻 `inode_unref` 丢掉 inode 引用(`sys_access.cpp:87-92`),之后只对着 `stat` 快照做权限裁决——`access_granted` 是个纯函数(`sys_access.cpp:41`),不碰任何运行时状态。

反直觉点(这章的次明星):**root(uid==0)在 R/W 上全放行,唯独 X_OK 要看 inode 有没有任一执行位**(`imode & 0111`)。这镜像 Linux `generic_permission` 的 root bypass——root 想执行一个文件,文件自己也得至少有一个 x 位。后果很具体:busybox 以 root 跑时,`access(R_OK/W_OK)` 几乎必过(文件不存在才会 `ENOENT`),但 `access(X_OK)` 对一个 `0644` 纯数据文件会真的返 `-EACCES`。这是 root **唯一**会被 `access` 拒的场景,文件头注释第 7-9 行直接点破了这一条:

```
busybox/gcc run as root, so R/W generally succeed; X requires an execute bit --
which is the one denial a root caller can still hit (access(X_OK) on a 0644 file).
```

(`sys_access.cpp:7-9`。root bypass 在 `sys_access.cpp:46-53`,X_OK 看执行位那行是 52。)

诚实边界:`access` 没有 ACL、没有 capabilities,只有 root bypass + 标准 owner/group/other 三元组(`sys_access.cpp:54-58`)。这是 Linux `generic_permission` 的最小子集,够 busybox/gcc 用,真要做多用户系统得扩。

### `getrandom`:glibc 启动的硬需求

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

### `pread64`:不推进 offset 不是靠锁,是靠接口签名

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

### `prlimit64`:「让 libc 满意」而非「真做资源管控」的标本

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

### `time` + `gettimeofday`:日志和 timestamp 要时间

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

## stub 的艺术:`-ENOSYS` 是正面信号,不是没做完

这是本章头号认知增量,值得单独一节慢慢讲——因为新手第一次看到 syscall 返 `-ENOSYS` 的本能反应是「内核坏了/没实现完」,恰恰相反。

### 关键背景:dispatch 兜底返 `-kEnosys`,不是裸 `-1`

先看 `syscall_dispatch`(`kernel/arch/x86_64/syscall.cpp:309`)对未注册号怎么处理:

```cpp
auto fn = cinux::arch::syscall_table[nr];
if (fn == nullptr) {
    // Unregistered syscall: return -ENOSYS (not bare -1) so a probing
    // libc (musl probes rseq/prlimit/...) can fall back gracefully.
    // musl treats -4095..-1 as -errno, so bare -1 would read as EPERM.
    cinux::lib::kprintf("[SYSCALL] unhandled syscall %u\n", static_cast<unsigned>(nr));
    return -cinux::kEnosys;
}
```

(`kernel/arch/x86_64/syscall.cpp:309-316`。)

为什么不返裸 `-1`?因为 musl/glibc 把 `-4095..-1` 这一段当 `-errno` 解读——裸 `-1` 被读成 `errno=EPERM`(1)。libc 以为「syscall 存在但被权限拒了」,走错路径直接报错;`-kEnosys`(38)才被读成「syscall 不存在」,libc 立刻 fallback。注释把这层 ABI 陷阱明写出来了。

这意味着:**就算不写任何 stub,大部分探测也能工作**——未注册号被 dispatch 兜底返 `-ENOSYS`,libc 照样降级。那为什么还要显式写 stub 文件?两个理由。

### 显式写 stub 的两个理由

**理由一:塞特定返回值让探测正确降级**——不是所有探测都吃 ENOSYS,有的探测要别的值才对(下一节讲 `set_robust_list` 就是返 0)。

**理由二:不刷日志**——未注册号每发一次,dispatch 兜底都 `kprintf("[SYSCALL] unhandled syscall N")`(`syscall.cpp:314`)刷一条日志。gcc 自举一轮会探上千次没注册的号,日志被刷爆。注册一个 stub 返同样的 `-ENOSYS`,信号一样、日志不刷——这是「在同一个正确信号下消除噪音」的工程取舍。

### 五个 stub:四个返 ENOSYS,一个返 0

`sys_linux_stubs.cpp` 文件头注释把每个 stub 的降级理由一行一条列出来(`sys_linux_stubs.cpp:6-12`):

```
- rseq(334)    -> -ENOSYS: glibc gives up the restartable-sequence path.
- clone3(435)  -> -ENOSYS: glibc falls back to clone/fork.
- set_robust_list(273) -> 0: the robust-futex probe is satisfied.  We do
  not truly clean up robust locks on exit, but no compile/load path uses
  them (pthread-only); a future pthread batch would wire the cleanup.
- sendfile(40) -> -ENOSYS: cp/copy tools fall back to read+write.
```

加上 `getcpu`(`sys_linux_stubs.cpp:29`),五个 stub 的决策表是:

| 号 | 返回值 | libc 收到后做什么 |
|---|---|---|
| `getcpu`(309) | `-ENOSYS` | glibc 放弃 per-CPU 亲和 hint,退回不带 NUMA 信息的实现 |
| `rseq`(334) | `-ENOSYS` | glibc 放弃可重启序列(rseq)的 lockless 路径,退回原子/锁 |
| `clone3`(435) | `-ENOSYS` | glibc 回退到老 `clone`(56)/`fork`(57) |
| `sendfile`(40) | `-ENOSYS` | busybox `cp`/`copy` 回退到 `read`+`write` 循环,功能照常 |
| `set_robust_list`(273) | **`0`** | **(例外)** robust-futex probe 通过,libc 认为内核支持 robust 锁 |

前四个是「功能型」stub——它们各自对应一个完整功能(per-CPU 亲和、rseq、新 clone、零拷贝 sendfile),Cinux 当前都没做,返 ENOSYS 让 libc 走老路。

`set_robust_list` 是**例外**,它是「探测型」stub。glibc 启动会探这个号,问「你支持 robust futex 吗?」——返 ENOSYS 会让 libc 把整条 robust 路径判废(连带影响 `pthread_mutex` 的 `ROBUST` 属性路径);返 0 才让 probe 通过,真清理留到将来 pthread 批次。源码注释明确承认没真做清理(`sys_linux_stubs.cpp:9-11`):

```
- set_robust_list(273) -> 0: the robust-futex probe is satisfied.  We do
  not truly clean up robust locks on exit, but no compile/load path uses
  them (pthread-only); a future pthread batch would wire the cleanup.
```

### 决策原则:返 0 还是返 ENOSYS,取决于探测语义

把上面五个 stub 的差别拎出来,就是这一章的认知脊柱:

- **探测型**(`set_robust_list`):返 0 满足探测,功能可后续补;
- **功能型**(`sendfile`/`getcpu`/`rseq`/`clone3`):返 `-ENOSYS` 让 libc 降级,功能不再走这条路径。

一行代码的区别(`return 0;` vs `return -kEnosys;`)背后是 ABI 协议——`set_robust_list` 返 0 等于「支持且成功」,后续若真用 robust futex 才踩空;返 `-ENOSYS` 是显式「不支持」。当前 Cinux 没 pthread,没人真用 robust futex,所以返 0 是「先让探测满意」的务实选择。

把这个原则推回 `prlimit64`:它不是 stub 也不是真实现,是策略型——「收下 new_rlim 忽略 + 返无限 limit」哄过 probe。三种应答方式(真给数据 / 返 ENOSYS 降级 / 返 0+无限哄过 probe)对应三类用户态需求,选哪种全看「libc 拿到这个值后行为对不对」。

> **ENOSYS 是 ABI 契约的一部分,不是失败。** 这句话是这一节的全部 punchline。新手看到 `-ENOSYS` 觉得是 bug,老手看到 `-ENOSYS` 觉得是「libc 和内核之间达成的降级协议」。返 `-EPERM`(裸 `-1` 被 musl 读成 EPERM)才是 bug——它让 libc 误判,直接报错不降级。

## stub 文件里的真实现:`tkill` / `setitimer` / `sched_getaffinity`

文件名叫 `sys_linux_stubs.cpp`——别被名字骗了。里面混着三个**完整真实现**,有正经逻辑。它们和 stub 同处一文件,因为都属于「Linux ABI 杂项/探测类」拼图,不是一个完整子系统,归在一起便于维护。读这文件要把 stub 和真实现分开看,否则会以为「这文件全是桩」。

### `tkill`:单线程模型下退化成 pid 查找

`tkill(200)` 的用户态需求来自 busybox sh 的作业控制——你按 Ctrl+C,信号从 PTY 进来,busybox sh 要把 `SIGINT` 转发给前台子进程(比如正在跑的 `ping`)。`tkill` 是「按 tid 发信号」,在 Linux 多线程模型里 tid 和 pid 不同(线程有自己的 tid)。

但 Cinux 任务**单线程**——tid 就是 pid。所以 `sys_tkill`(`sys_linux_stubs.cpp:53`)直接复用 `sys_kill`(62)的 pid→Task 查找 + `signal_send` 路径:

```cpp
int64_t sys_tkill(uint64_t tid, uint64_t sig, uint64_t, uint64_t, uint64_t, uint64_t) {
    auto* t = cinux::proc::signal_find_task_by_pid(static_cast<int>(tid));
    if (t == nullptr) {
        return -cinux::kEsrch;
    }
    return cinux::proc::signal_send(t, static_cast<cinux::proc::Signal>(sig));
}
```

(`sys_linux_stubs.cpp:53-59`。)

注释写清楚了「为什么能复用」——`tid == pid`(`sys_linux_stubs.cpp:51`),不是默默 alias。教学点:单线程简化让很多 Linux 多线程语义退化成 pid 查找,`tkill`/`tgkill` 这些线程定向信号在单线程模型下和 `kill` 等价,代码复用比复制一份逻辑更对。

### `setitimer`:真实现分散在三个文件里

`setitimer(38)` 是这章另一个反直觉明星——它是个**真实现**,但真实现的兑现不在 syscall handler 里。

用户态需求:busybox `ping` 每秒发一个 echo 包靠它——`setitimer(ITIMER_REAL, 1s 间隔)`,每秒 `SIGALRM` 一到就发下一个包。如果 `setitimer` 是 stub 返 0 不真做,`ping` 会发一个包就卡住等不到下一拍。

`sys_setitimer`(`sys_linux_stubs.cpp:93`)的 handler 主体看着朴素:把 Linux `itimerval{it_interval, it_value}` 拷进来、转纳秒、存进当前 Task 的 `itimer_real_value_ns`/`interval_ns` 字段、return 0:

```cpp
{
    cinux::proc::InterruptGuard guard;
    self->itimer_real_value_ns    = timeval_to_ns(newv.it_value);
    self->itimer_real_interval_ns = timeval_to_ns(newv.it_interval);
}
return 0;
```

(`sys_linux_stubs.cpp:122-127`。)

看着像个赋值 stub——但**真正的定时逻辑在 `signal.cpp:296` 的 `itimer_real_tick`**。PIT IRQ0 每 tick 调它,遍历所有 Task 递减 `value_ns`,到点从 `interval_ns` reload 并 queue `SIGALRM`。下面是它的结构(细节省略,完整版在源码):

```cpp
void itimer_real_tick(uint64_t delta_ns) {
    Task* expired[kMaxExpired];
    int   nexpired = 0;
    {
        auto g = g_registry_lock.irq_guard();   // walk the task registry
        for (Task* t = g_registry_head; t != nullptr; t = t->registry_next) {
            if (t->itimer_real_value_ns == 0) continue;        // disarmed
            if (t->itimer_real_value_ns > delta_ns) {
                t->itimer_real_value_ns -= delta_ns;
            } else {
                t->itimer_real_value_ns = t->itimer_real_interval_ns;  // reload
                expired[nexpired++] = t;                               // collect, signal later
            }
        }
    }
    for (int i = 0; i < nexpired; ++i) {
        signal_send(expired[i], Signal::kSigalrm);   // queue SIGALRM
    }
}
```

(`signal.cpp:296-329`,上面省略了 lock 注释和 expired 上限保护。)

所以「真实现」的兑现不在 syscall handler 里,而在**中断驱动的 tick 回调**里。读 `sys_linux_stubs.cpp` 这个文件要把三处合起来看:`sys_setitimer`(写 Task 字段)+ `process.hpp:313-315`(字段本身 + 「Aligned 64-bit rw atomic (TSO); cross-CPU race vs setitimer is benign」那条注释)+ `signal.cpp:296`(PIT tick 递减)。否则会以为 `setitimer` 只是赋值,功能跑不起来。

并发细节:字段更新套 `InterruptGuard`(`sys_linux_stubs.cpp:123`),防 PIT tick 在写到一半时打断读到半更新的 `(value, interval)` 对。`process.hpp:313` 的注释承认这是 best-effort——cross-CPU 的 race 在 TSO + 64 位对齐读写下属「良性」(missed/repeated SIGALRM 都不致命,`signal.cpp:303`)。

诚实声明:`setitimer` 只支持 `ITIMER_REAL`(busybox/glibc 只探 REAL),`ITIMER_VIRTUAL`/`ITIMER_PROF` 返 `-EINVAL`(`sys_linux_stubs.cpp:96-98`)。

> **延伸调试(方法论提示,不是已知 bug 清单):** stub 升级到真实现往往会**连带暴露**信号递交、栈对齐、PIT 走时这几条埋着的暗线——这是 timer 这类「跨子系统」syscall 的典型副作用。读者真把 `setitimer` 从 stub 改成真实现时,要留意的不是单点 bug,而是整条递交通路:`schedule_blocked` 在可中断 IO 等待时会不会看 pending signal、`signal_setup_frame` 给 handler 入口准备的 RSP 对齐能不能过 SSE `movaps`(`#GP` 被映射成 `SIGILL` 是常见症状)、PIT 模式寄存器(mode 3 vs mode 2)的边沿数算对没。这些都是「stub 阶段藏着的 bug 要等真实现有 1s 参照才显形」的典型场景——你以为在加一个 timer,实际挖出的是信号递交、栈对齐、PIT 走时三条暗线。(当前 Cinux tree 里这些路径已经修好能跑 ping,本章不展开具体提交史——留给读者真做时自己撞。)

### `sched_getaffinity`:返字节数,不是返 0

busybox `nproc` 和 glibc 探它学在线 CPU 集。`sys_sched_getaffinity`(`sys_linux_stubs.cpp:135`)从 `g_acpi_info.cpu_count`(BSP + AP 数,MADT 解析得来)算掩码,每个在线 CPU 一位置位:

```cpp
for (uint32_t i = 0; i < n && i < sizeof(buf) * 8; ++i) {
    buf[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
}
...
return static_cast<int64_t>(bytes);
```

(`sys_linux_stubs.cpp:143-153`。)

这里有个**返回值契约**很容易踩坑——Linux 原始 syscall 返回「写入的字节数」(非 0),glibc/musl 的 wrapper 才把非负翻译成 0。所以这里 `return bytes` 而不是 `return 0`,否则 libc 解析会错。新手写 stub 容易直接 `return 0` 觉得「成功」——错了,那会让 glibc 拿到「写了 0 字节」的信号,以为没 CPU 在线。

`pid` 参数被 `/*pid*/` 忽略——Cinux 全局单一 affinity,没有 per-process affinity 概念。注释明说「why ignored 比假装校验更诚实」(`sys_linux_stubs.cpp:130-134`)。这是个诚实工程:与其写一堆假装检查 pid 的代码,不如直接说「只有一个全局 affinity」。

## `cinux_exit`:暴露给 ring3 的 QEMU 退出口(不是 `sys_exit`)

独立小节讲这个 Cinux 专有偏离。它和前面十四个号不一样——它不是 Linux ABI 拼图的一部分,是 Cinux 为了自动化验证**自己加**的一个号。

### 问题背景:用户态怎么让 QEMU 退出

buildroot 用户态(ash + 测试脚本)跑完测试要给 CI 一个 pass/fail 码——可用户态不能直接 `outb`(没 `iopl`/`ioperm`),`sys_reboot` 是 `-EPERM` 桩(busybox init 探它但不真做)。所以 `isa-debug-exit`(QEMU 的退出口设备,port 0xf4)这条路径只能靠一个**专有 syscall** 暴露给 ring3。

实现极其朴素(`sys_cinux_exit.cpp:27`):

```cpp
int64_t sys_cinux_exit(uint64_t code, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    // Never returns: QEMU exits with (code<<1)|1. Truncated to uint32_t to
    // match the device's iosize=4 (and the outl width the test harness uses).
    cinux::io::io_outl(kQemuExitPort, static_cast<uint32_t>(code));
    return 0;  // unreachable -- QEMU has exited
}
```

(`sys_cinux_exit.cpp:24-29`,真正写端口那行在 27。)

一条 `io_outl(0xf4, code)` 终止 QEMU,退出码是 `(code<<1)|1`(0→QEMU 退 1=`qemu_test_wrapper.sh` 映射 SUCCESS;非零→退 3+ = 失败)。它复用了测试框架(`kernel/test/main_test.cpp`)和 panic 路径(`exception_handlers.cpp`)在内核态做同一件事的原语,只是把它**暴露给 ring 3**。

### 头号混淆点:`cinux_exit` ≠ `sys_exit`/`sys_exit_group`

这是这一节必须讲清的——这俩名字像,量级完全不同。

- `sys_exit`(60)/`sys_exit_group`(231)是**进程卷**的事:把当前 task 标 `Zombie`、编码 waitpid status word、唤醒父进程 reap。**当前 ring3 程序**结束了,但内核和其他 task 还在跑。
- `sys_cinux_exit`(221)跟进程没关系——它 `outl` 端口让**整个 QEMU 进程**退出。不是退出程序,是退出**整台虚拟机**。

混淆了会把 CI gate 写成「程序退出就 PASS」,实际要的是「QEMU 退出码对」——前者任何一个 `exit(0)` 都触发,后者只有 `cinux_exit(0)` 才触发。头文件注释把这俩的区别点透(`sys_cinux_exit.hpp:5-14`):

```
Cinux-custom syscall (SYS_cinux_exit = 221). Lets a Buildroot userland
(ash + a test script) terminate the QEMU run with a pass/fail code that CI
gates on: ... Userspace cannot outb directly (no iopl/ioperm), and
sys_reboot is a -EPERM stub, so this syscall is the ONLY userspace ->
isa-debug-exit path.
```

### 占号问题:221 在 Linux 是 `fadvise64`,不是 fcntl

Linux x86_64 表里 221 是 `fadvise64`(`syscall_64.tbl` 里那行 `221 common fadvise64 sys_fadvise64`)——不是 fcntl。fcntl 在 x86_64 是 72(`syscall_nums.hpp:52` 也对:`SYS_fcntl = 72`)。Cinux 把 221 占为专有退出,实际占的是 Linux `fadvise64` 的号。

为什么不撞自己的 fcntl?因为 Cinux fcntl 走 `SYS_fcntl=72`(`syscall_nums.hpp:52`),musl/glibc 都走 wrapper 发 72 不发裸 221。这是**有意的、文档化的 ABI 偏离**——用户态触发器 `tools/musl/cinux-exit.c` 手动 `#define SYS_cinux_exit 221` 并注释「Keep in sync with kernel/syscall/syscall_nums.hpp」(`cinux-exit.c:19-21`),因为 musl 的 `sys/syscall.h` 没这个号:

```c
/* Cinux-custom syscall; not in musl's sys/syscall.h. Keep in sync with
 * SYS_cinux_exit in kernel/syscall/syscall_nums.hpp. */
#define SYS_cinux_exit 221
```

(`cinux-exit.c:19-21`。)

诚实边界:理论上若有 Linux 程序硬发裸 221 期望 `POSIX_FADV_*` 预读建议语义(那才是 Linux 221 的真身),会被 `cinux_exit` 误触发 QEMU 退出。**但 musl/glibc 的 `posix_fadvise` wrapper 也走自己的号路径,不会发裸 221**——所以实际不撞。这是个已知但未深究的边界。

### CI gate 链:busybox init → 测试脚本 → cinux-exit → QEMU 退出码

整条 CI gate 链是这样:

```
busybox init → inittab ::once → cinux-usability-test.sh → cinux-exit [code]
   → sys_cinux_exit(221) → io_outl(0xf4) → QEMU 退出 (code<<1)|1
```

`cmake/qemu.cmake:604-618` 把这条链立成 `run-buildroot-usability` target——`-device isa-debug-exit,iobase=0xf4,iosize=0x04` 把 QEMU 的退出设备挂上,CI 跑完看 QEMU 进程退出码。

诚实边界:**生产环境(run target,无 isa-debug-exit 设备)下这个 syscall 变 no-op**——`io_outl` 写一个没人听的端口,什么都不会发生。这是「Cinux 不是 Linux」的一处必要偏离——为了自动化验证必须有一个 Linux 没有的 syscall,但它只服务于 CI,不影响 POSIX 兼容(真 Linux 程序不会发 221 当 fadvise64,因为 musl/glibc wrapper 走自己的号路径)。

## 验证(教程即验证)

A 档验证段——跟 059/073 同款「客观陈述看到什么」。本章做了两条验证路径。

### 路径一:静态核实(本章做了的)

**号都注册了**:`kernel/arch/x86_64/syscall.cpp` 把这批 syscall 全部 `syscall_register` 进了 dispatch 表——`SYS_pread64` 在 117 行、`SYS_access` 在 232 行、`SYS_sendfile`/`SYS_gettimeofday`/`SYS_getcpu`/`SYS_sched_getaffinity`/`SYS_set_robust_list`/`SYS_prlimit64`/`SYS_getrandom`/`SYS_rseq`/`SYS_clone3`/`SYS_time` 在 235-244 行,`SYS_setitimer`/`SYS_tkill` 在 204-205 行,`SYS_cinux_exit` 在 192 行。

**号都对齐 Linux x86_64**:`syscall_nums.hpp:132-141` 这一批号逐个对 Linux x86_64 表——`sendfile=40`、`gettimeofday=96`、`set_robust_list=273`、`prlimit64=302`、`getcpu=309`、`getrandom=318`、`rseq=334`、`clone3=435`、`time=201`、`sched_getaffinity=204`,和 Linux UAPI 完全一致。`access=21`(`syscall_nums.hpp:77`)、`pread64=17`(`syscall_nums.hpp:40`)、`setitimer=38`、`tkill=200` 是更早就在表里的,本章只是归类。`cinux_exit=221` 是 Cinux 专有(占 Linux `fadvise64` 号,文档化偏离)。

**handler 逻辑与源码注释一致**:逐文件核实——`sys_access.cpp` 的 root bypass 在 46-53 行、X_OK 看执行位;`sys_getrandom.cpp` 的 flags 忽略在签名第 22 行 + 注释 7-8;`sys_pread64.cpp:25-44` 的 offset 用调用者的、无 `file->offset +=`;`sys_prlimit64.cpp:33-42` 的 pid/resource/new 全忽略、old 写 `{~0, ~0}`;`sys_linux_stubs.cpp` 的四个 ENOSYS + 一个返 0 在 29-47、三个真实现(tkill/setitimer/sched_getaffinity)在 53-154。

**dispatch 兜底返 `-kEnosys` 不是裸 `-1`**:`kernel/arch/x86_64/syscall.cpp:309-316` 核实过,注释把 musl 的 errno 解读陷阱明写。`kEnosys = 38`(`errno.hpp:45`)。

### 路径二:真负载佐证(可引用机制测和真程序)

**busybox `ping` 跑起来**:靠 `setitimer` 每秒 `SIGALRM`——若 `setitimer` 是 stub 返 0 不真做,`ping` 发一个包就卡住。能跑就是 `sys_setitimer` + `signal.cpp:296` 的 `itimer_real_tick` 真被调到的活证据。

**busybox `nproc` 报真实 CPU 数**:靠 `sched_getaffinity` 从 `g_acpi_info.cpu_count` 算掩码——若 stub 返错字节数,`nproc` 会报 0 或乱码。

**busybox sh 的 Ctrl+C 转发**:靠 `tkill` 把 `SIGINT` 发给前台子进程——若 `tkill` 是 stub,信号发不出去,Ctrl+C 失灵。

**带 SSP 的 musl 程序能启动**:`getrandom` 被 glibc/musl 启动用来填 canary——若返 ENOSYS 或不真给字节,带 SSP 的程序会启动失败或 canary 全零。hello/busybox 都能跑(059/073 立过)就是 `getrandom` 真给字节的间接证据。

> **本章止于静态核实。** 源码头注释和 `cmake/qemu.cmake:604-618` 的 gate 文档声称这些 syscall 被用到,但「声称」≠「实测」——本章没在 QEMU 上跑 `run-buildroot-usability` 端到端坐实「musl/glibc/busybox 真发这个号、真命中这个 handler」(那需要 strace 或串口日志 grep)。lab 留给读者做这件事。

## 诚实的边界

把本章声明的几条刻意简化列清楚——这是 A 档的硬要求。

1. **`prlimit64` 收下 `new_rlim` 但完全忽略**,任何 `setrlimit` 都是静默 no-op——`RLIMIT_NOFILE` 不真限 fd 表、`RLIMIT_STACK` 不限栈、`RLIMIT_CORE`/`AS`/`DATA` 全无限。glibc malloc 拿到「无限」按默认走,正好是 Cinux 想要的;若未来真要做资源管控要补 enforcement path。

2. **`set_robust_list` 返 0 哄过 probe,但没真清理 robust futex**——等 pthread 批次才接,当前没 pthread 没人真用,返 0 是「先让探测满意」的务实选择(`sys_linux_stubs.cpp:9-11` 明确承认)。

3. **`sendfile`/`rseq`/`clone3`/`getcpu` 是纯 stub**(返 `-ENOSYS`),后续真要做时再展开——`rseq` 给 perf、`clone3` 给 pthread、`sendfile` 给零拷贝。

4. **`access` 无 ACL/capabilities**,只有 root bypass + 标准 owner/group/other——镜像 Linux `generic_permission` 的最小子集。

5. **`getrandom` 不是 CSPRNG**——boot 后状态固定(xoshiro 流),对 ASLR/canary 够用,做密码学密钥材料不合规。源码头注释自己声明了 honest scope。

6. **`sys_pread64` 对 pipe/pty 等 non-seekable fd 不返 `ESPIPE`**(Linux 标准行为),而是坍缩成 `EBADF`——因为 Cinux 没把 stdin/console 当 pread 目标,`sys_pread64.cpp:41-43` 明确承认这点偏离。

7. **`CLOCK_REALTIME` 的 RTC 只在开机读一次粗秒**(秒级精度),之后全靠 HPET 增量精化——「drift correction」不重读 RTC(慢),反直觉但正确。

8. **`monotonic_ns()`(HPET-available-else-PIT 那个同款实现)在 kernel 树里复制了至少五份**——`sys_clock_gettime.cpp:40`、`sys_select.cpp:52`、`sys_nanosleep.cpp:30`、`poll_core.cpp:39`、`proc/timer_queue.cpp:45`,没抽公共 API(`kernel/time/monotonic.hpp` 之类的)。是 Cinux 还没做的时间子系统统一,是个干净的 refactor hook,可串下一章。

9. **`SYS_cinux_exit=221` 占了 Linux `fadvise64` 号**(不是 fcntl——fcntl 是 72)。当前 musl/glibc 走 `SYS_fcntl=72` wrapper 不发裸 221 所以不撞;理论上若有 Linux 程序硬发裸 221 期望 `POSIX_FADV_*` 预读建议语义,会触发 QEMU 退出,这是已知但未深究的边界。

## 这章没做的

- **没在 QEMU 上跑 buildroot usability gate 端到端坐实**:本章是静态源码核实,没拿 strace/串口日志 grep 确认「musl/glibc/busybox 真发这些号、真命中这些 handler」。lab 留这条给读者。
- **`prlimit64` 没有 enforcement path**:返无限是策略,不是真做资源管控。若未来要做 RLIMIT_NOFILE 真限 fd 表,要补 `RLIMIT_*` 检查点散布在 fd alloc/mmap/stack grow 路径上。
- **robust futex 没真清理**:`set_robust_list` 返 0 是 probe 满足,真清理留 pthread 批次。
- **`monotonic_ns` 没抽公共 API**:时间子系统统一化未做(五份拷贝散在 syscall/proc 各处),是干净的 refactor hook。
- **`getrandom` 没接连续重采样**:boot 一次 seed 后状态固定,做密码学得接硬件 RNG 的连续熵。

## 小结

- **判断一个号该怎么应答,看 libc 拿到返回值后的行为**——这是这章的认知脊柱。返 0(满足探测)vs 返 ENOSYS(让降级)vs 返数据(真给字节)vs 收下忽略(策略型),四选一全看「libc 拿到这个值后行为对不对」,不是「实现难不难」。
- **`-ENOSYS` 是正面信号**:dispatch 兜底返 `-kEnosys`(38),不返裸 `-1`(被 musl 读成 EPERM)。`getcpu`/`rseq`/`clone3`/`sendfile` 返 ENOSYS 是对的——libc 收到就降级。`set_robust_list` 返 0 也是对的——它是探测型,返 ENOSYS 反而让 libc 判废 robust 路径。
- **`sys_linux_stubs.cpp` 不全是 stub**:`tkill`/`setitimer`/`sched_getaffinity` 是完整真实现,只是归在「ABI 杂项」文件里。`setitimer` 的真实现分散在 syscall handler(Task 字段)+ `process.hpp`(字段)+ `signal.cpp:296`(PIT tick)三处,要合起来看。
- **`pread64` 不推进 offset 不是靠锁或回滚**,是靠接口签名——`InodeOps::read(inode, offset, ...)` 的 offset 是入参,`sys_pread64` 压根没碰 `file->offset`,对照 `sys_read` 才显式 `+=`。
- **`cinux_exit` 跟 `sys_exit` 是两件完全不同量级的事**:前者 `outl(0xf4)` 让整个 QEMU 进程退出(CI gate),后者把 task 标 Zombie(进程卷)。混淆会把 CI gate 写错。
- **诚实边界**:九个真实现 + 五个 stub + 一个专有偏离是务实——`prlimit64` 不强制、`set_robust_list` 没真清理、`getrandom` 不是 CSPRNG、`access` 无 ACL、`pread64` 偏离 ESPIPE、`monotonic_ns` 五份拷贝、`cinux_exit` 占 Linux `fadvise64` 号——都写明白了。
