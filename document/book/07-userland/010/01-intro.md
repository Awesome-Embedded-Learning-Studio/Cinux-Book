---
title: 01 · Linux ABI 拼图:让 glibc/musl/busybox 跑起来
---

# Linux ABI 拼图:让 glibc/musl/busybox 跑起来

> 059 让 musl 静态 hello 跑通,073 让静态 busybox 14 个 applet 跑通——这两章把「能让一个真程序起来」的 ABI 地基铺好了:Linux x86_64 的 syscall 号表、负 errno 的返回约定、铺满 auxv 的初始栈、busybox 试金石和它用到的 `getdents64`/`chmod`/`dup`/`fcntl`/`nanosleep`/`socket` 那一大坨。这一章接的不是另一批真程序,是另一批**契约**:让 glibc 和更激进的 musl 程序在启动和运行各阶段不被一个缺掉的 ABI 卡住——内核要么真给数据、要么诚实地返 `-ENOSYS` 让 libc 优雅降级。这一章一共讲十**四**个 Linux ABI 号 + 一个 Cinux 专有的偏离(`cinux_exit`),其中九个真实现、五个是 stub,但都不是「清单」,是 glibc 启动会挨个探过去的一条 ABI 谱。
>
> 验证口径:教程即验证。本章核实的是源码侧事实——这十五个号都注册进了 dispatch 表、号都和 Linux x86_64 对齐(只有 `cinux_exit` 占了 Linux `fadvise64` 的号,文档化偏离)、handler 逻辑和源码注释里声明的语义一致、dispatch 兜底返的是 `-kEnosys` 不是裸 `-1`。诚实的边界先放在台面上:九个真实现(`access`/`getrandom`/`pread64`/`prlimit64`/`time`/`gettimeofday`/`tkill`/`setitimer`/`sched_getaffinity`)、五个是返 `-ENOSYS` 或返 0 哄过的探测 stub(`getcpu`/`rseq`/`clone3`/`sendfile`/`set_robust_list`),外加一个 Cinux 专有的 `cinux_exit`。这不是残缺,是务实——内核**判断一个号该真实现还是 stub 的唯一依据,是 libc 拿到这个返回值后行为对不对**,不是「这个 syscall 容不容易实现」。这章就是把这条判断原则讲成可读的东西。

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
