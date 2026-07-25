---
title: 04 · stub 文件里的真实现:`tkill` / `setitimer` / `sched_getaffinity`
---

# stub 文件里的真实现:`tkill` / `setitimer` / `sched_getaffinity`

文件名叫 `sys_linux_stubs.cpp`——别被名字骗了。里面混着三个**完整真实现**,有正经逻辑。它们和 stub 同处一文件,因为都属于「Linux ABI 杂项/探测类」拼图,不是一个完整子系统,归在一起便于维护。读这文件要把 stub 和真实现分开看,否则会以为「这文件全是桩」。

## `tkill`:单线程模型下退化成 pid 查找

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

## `setitimer`:真实现分散在三个文件里

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

## `sched_getaffinity`:返字节数,不是返 0

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
