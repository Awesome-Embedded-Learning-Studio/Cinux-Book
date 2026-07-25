---
title: 06 · 收尾:验证、诚实边界、没做的与小结
---

# 收尾:验证、诚实边界、没做的与小结

## 验证(教程即验证)

验证口径——跟 059/073 同款「客观陈述看到什么」。本章做了两条验证路径。

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

把本章声明的几条刻意简化列清楚——这是诚实边界的硬要求。

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
