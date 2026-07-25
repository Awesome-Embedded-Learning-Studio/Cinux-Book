---
title: 03 · stub 的艺术:`-ENOSYS` 是正面信号,不是没做完
---

# stub 的艺术:`-ENOSYS` 是正面信号,不是没做完

这是本章头号认知增量,值得单独一节慢慢讲——因为新手第一次看到 syscall 返 `-ENOSYS` 的本能反应是「内核坏了/没实现完」,恰恰相反。

## 关键背景:dispatch 兜底返 `-kEnosys`,不是裸 `-1`

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

## 显式写 stub 的两个理由

**理由一:塞特定返回值让探测正确降级**——不是所有探测都吃 ENOSYS,有的探测要别的值才对(下一节讲 `set_robust_list` 就是返 0)。

**理由二:不刷日志**——未注册号每发一次,dispatch 兜底都 `kprintf("[SYSCALL] unhandled syscall N")`(`syscall.cpp:314`)刷一条日志。gcc 自举一轮会探上千次没注册的号,日志被刷爆。注册一个 stub 返同样的 `-ENOSYS`,信号一样、日志不刷——这是「在同一个正确信号下消除噪音」的工程取舍。

## 五个 stub:四个返 ENOSYS,一个返 0

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

## 决策原则:返 0 还是返 ENOSYS,取决于探测语义

把上面五个 stub 的差别拎出来,就是这一章的认知脊柱:

- **探测型**(`set_robust_list`):返 0 满足探测,功能可后续补;
- **功能型**(`sendfile`/`getcpu`/`rseq`/`clone3`):返 `-ENOSYS` 让 libc 降级,功能不再走这条路径。

一行代码的区别(`return 0;` vs `return -kEnosys;`)背后是 ABI 协议——`set_robust_list` 返 0 等于「支持且成功」,后续若真用 robust futex 才踩空;返 `-ENOSYS` 是显式「不支持」。当前 Cinux 没 pthread,没人真用 robust futex,所以返 0 是「先让探测满意」的务实选择。

把这个原则推回 `prlimit64`:它不是 stub 也不是真实现,是策略型——「收下 new_rlim 忽略 + 返无限 limit」哄过 probe。三种应答方式(真给数据 / 返 ENOSYS 降级 / 返 0+无限哄过 probe)对应三类用户态需求,选哪种全看「libc 拿到这个值后行为对不对」。

> **ENOSYS 是 ABI 契约的一部分,不是失败。** 这句话是这一节的全部 punchline。新手看到 `-ENOSYS` 觉得是 bug,老手看到 `-ENOSYS` 觉得是「libc 和内核之间达成的降级协议」。返 `-EPERM`(裸 `-1` 被 musl 读成 EPERM)才是 bug——它让 libc 误判,直接报错不降级。
