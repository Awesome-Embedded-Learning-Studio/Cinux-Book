---
title: 02 · 代码路线:PID 1、execve 保 pid、rt_sigtimedwait、/dev/console 与 EFAULT
---

# 代码路线:PID 1、execve 保 pid、rt_sigtimedwait、/dev/console 与 EFAULT

## 主线一:PID 1 是怎么来的——init 线程入口领号

先破一个想当然:**"init 是 fork 出来的"是错的**。

Cinux 的内核线程(`task_builder.cpp` 里建的)生来 `pid=0`——它压根不碰全局 PID 分配器 `g_pid_alloc`。`g_pid_alloc` 只在一个地方发号:`fork()`。那么在 boot 阶段,`fork()` 还一次都没被调用过,`g_pid_alloc.alloc()` 第一次被调用时,返回值必然是 1。

谁来做这"第一次调用"?就是 init 线程自己。在 [init.cpp](../../../kernel/proc/init.cpp) 的 `kernel_init_thread()` 入口:

```cpp
auto* self = Scheduler::current();
// 这个 kthread 现在就是 PID 1 —— Linux 的 kernel_init 模型。
// TaskBuilder 让内核线程停在 pid=0(它们从不碰 g_pid_alloc),
// 而 boot 到这里之前没有任何 fork(),所以第一次 alloc() 返回 1。
if (self != nullptr) {
    self->pid          = g_pid_alloc.alloc();
    self->tgid         = self->pid;
    self->group_leader = self;
}
```

三行,把当前内核线程从"匿名 kthread(pid=0)"扶正成"PID 1、自成一组、自己是 group leader"。这就是 init。

> 笔者得在这里插一句:这一段推翻了一个早先的误判。旧交接说"重排 `start_poll_driver` 的顺序,让 init 拿到 PID 1"。实测证伪——`start_poll_driver` 里的 `net_poll` 也是一个 kthread,它也只拿 tid、pid 留 0,**重排它对 PID 分配毫无影响**。根因是"kthread 不碰分配器",不是"谁先启动"。正解只能是 init 线程入口主动 `alloc()`。这个坑值得记一笔:**读交接文档时,见到"调换某段顺序就能拿到 pid"这种说法,先去查分配器到底在哪发号**——别信顺序,信机制。

## 主线二:`execve` 保 pid——init 线程直接 exec /sbin/init

领到 PID 1 之后,init 线程要做的事是:加载真正的 `/sbin/init`(busybox)并切到用户态。这里有个关键不变式:**`execve` 换的是程序映像,不是 pid**。所以 busybox init 接过映像的同时,也接过了 PID 1 这个身份。

非 GUI 构建里,`launch_userspace()` 的实现在 [shell_launch.cpp](../../../kernel/proc/shell_launch.cpp),改写成了"init 线程亲自 execve、不 fork"的模式:

```cpp
void launch_userspace() {
    auto* self = Scheduler::current();
    // execve 要求 addr_space 非空(内核线程生来没有),装一个新的。
    // execve 会先清掉旧映射,所以内核线程不会把任何用户映射漏进 init。
    self->addr_space = new cinux::mm::AddressSpace();

    const char* path   = "/sbin/init";
    const char* argv[] = {"init", nullptr};  // basename "init" -> busybox init applet
    const char* envp[] = {nullptr};
    launch_user_program(path, argv, envp);   // execve + jump_to_usermode; 不返回
}
```

两件事要拎出来:

- **不 fork**。旧实现是"fork 一个子进程,子进程 exec /bin/sh"。那样 shell 是 PID 2,init(父)是 PID 1——shell 退化成一个普通进程,失去了"init respawn 我"这层关系。现在 init 线程**自己** execve,它就是 PID 1 的 busybox init,往后由 busybox init 自己 vfork 出 `/bin/sh`(PID 2)、盯着它、死了再 respawn。
- **argv[0] = `"init"`**。busybox 是个多合一二进制,靠 `argv[0]` 的 basename 分派到具体 applet。`/sbin/init` 是 busybox 的一个硬链接,`argv[0]="init"` 让 busybox 跑 `init` applet,而不是默认的 `sh`。

`launch_user_program` 内部就是 `execve` + `jump_to_usermode`,**不返回**。这一条不返回,直接引出下一个必须改的地方。

### 顺带:USB 初始化得挪到 launch_userspace 前面

[init.cpp](../../../kernel/proc/init.cpp) 里,`launch_userspace()` 之后的代码在非 GUI 构建里**永远不会执行**——因为 `launch_userspace` 内部 `execve`+跳用户态,init 再也不回内核线程这条控制流。所以原本放在 `launch_userspace()` 之后的 `usb::init()`,得挪到它前面:

```cpp
// USB 输入得在 launch_userspace 之前 arm 好——非 GUI 的 launch_userspace
// execve /sbin/init 后不返回,放它后面的代码永远跑不到。
cinux::drivers::usb::init();

launch_userspace();   // 非_gui:execve /sbin/init,不返回

// 非_gui 构建里到不了这里。留着给将来某种"会返回的 launch_userspace"兜底。
Scheduler::exit_current();
```

GUI 构建不受影响——它的 `desktop_launch.cpp` 会另起一个 `gui_worker` 线程,init 线程照常往下走。§14 的双实现,CMake 按构建类型只链一个,源码里不写 `#ifdef`。

> 这一对实现(`shell_launch.cpp` / `desktop_launch.cpp`)是 §14「一个接口、两套实现、CMake 选链」的范本。本章只动非 GUI 那份;GUI 的桌面启动不在这一章的范围。

## 主线三:rt_sigtimedwait 不能真阻塞——否则死锁

busybox init 跑起来之后,它的主循环长这样(简化):**用 `rt_sigtimedwait` 等信号,靠它的返回值去决定"现在要不要 respawn sh、要不要 reap child"**。

这就埋了一个反直觉的雷。先看咱们在 [sys_signal.cpp](../../../kernel/syscall/sys_signal.cpp) 里怎么实现 `sys_rt_sigtimedwait`:

```cpp
// busybox init 的主循环在这里轮询 SIGCHLD。没有匹配的 pending 信号时返 -EAGAIN——
// init 把它当超时,转头去检查 respawn 动作(fork /bin/sh)和 reap child。
// busybox init 是靠 sigtimedwait 的"返回"来驱动 respawn 的,所以一个纯阻塞的实现
// (只有信号能唤醒)在还没有任何 child 被 fork 出来时会死锁。
// RR 调度器的时间片保证这个循环不会饿死 sh。
SigSet avail = task->sig_pending & wait_set;
if (avail != 0) {
    // 取一个 pending 信号返回
    ...
}
// avail == 0:返 -EAGAIN
```

为什么"纯阻塞"会死锁?把时序走一遍就明白:

1. busybox init 刚 execve 起来,**还没 fork 任何 child**,所以没有 SIGCHLD。
2. 它进主循环,调 `rt_sigtimedwait` 等信号。
3. 如果这个 syscall **真睡死**(只有来信号才唤醒),那它就挂在那儿——因为此刻根本没有信号。
4. 它不返回 → 不检查 respawn → `/bin/sh` 永远不被 fork 出来 → 也就永远不会有 child 退出 → 永远不会有 SIGCHLD → 它永远不醒。

死锁,而且是那种"系统看起来启动了、但什么也做不了"的死锁。

正解不是"加个超时"(那是 follow-up),而是最朴素的:**没有 pending 信号就立刻返 `-EAGAIN`**。busybox init 拿到 `-EAGAIN`,当成"超时了",转身去检查"该 respawn 了吗、有 child 要 reap 吗",然后下一轮再来。它本质是个**靠 `rt_sigtimedwait` 返回值驱动的轮询循环**,你给它 `-EAGAIN`,它就转起来了。RR 调度器的时间片保证 init 不会占着 CPU 不放、把 sh 饿死。

> 这个坑的教训是:**别假设一个"等信号"的 syscall 就该阻塞**。得看调用方拿它的返回值干嘛——busybox init 拿返回值当事件循环的 tick,你阻塞了,事件循环就停了。咱们把 `Task::sigwait_blocked` 这个唤醒钩子在 `signal_send()` 里留好了(精确 opt-in,只对真在 sigtimedwait 里的任务生效,不碰 futex/waitpid),将来要做"真阻塞 + 超时"的版本时,钩子现成的。

## 主线四:/dev/console 得会读、会答 ioctl

busybox init 起来后第一件事是 `open("/dev/console")`,把它 dup 成 0/1/2,再 `setsid` + `TIOCSCTTY` 挂控制终端。它 fork 出的 ash,判断"我是不是交互式终端"靠 `isatty`——本质是发一个 `TCGETS` ioctl;读命令行靠 `read`。这些全打到 `/dev/console` 这个 inode 的 ops 上。

可 064 的 DevFS 里,`/dev/console` 的 `ConsoleDevOps` 只接 **write**(往串口 sink 输出),**read 返回错、ioctl 不答**。于是:

> ash 发 `TCGETS` → 没人答 → `isatty` 返回 0 → ash 判定"我不是交互终端" → 当场退出 → busybox init 发现 child 死了 → respawn 一个新 ash → 新 ash 又退出 → ……

死循环,串口上看到的就是 init 不停地 respawn、ash 不停地秒退。

修法是给 `/dev/console` 接上真 console TTY 的后端,但**不能把 console TTY 的依赖直接塞进 `devfs.cpp`**——那样 DevFS 就没法在 host 上单测了(064 章立过的并行栅栏)。所以走注入:在 [devfs.hpp](../../../kernel/fs/devfs/devfs.hpp) 抽一个纯接口 `ConsoleInput`(只有 `read` + `ioctl` 两个虚函数),`DevFs` 构造收一个可选的 `ConsoleInput*`(默认 `nullptr`):

```cpp
/// /dev/console 的可选 read/ioctl 后端。注入 nullptr 时 read/ioctl 返 NotImplemented,
/// DevFs 保持 host 可测(不依赖 console_tty)。
class ConsoleInput {
public:
    virtual ~ConsoleInput() = default;
    virtual cinux::lib::ErrorOr<int64_t> read(void* buf, uint64_t count) = 0;
    virtual cinux::lib::ErrorOr<int64_t> ioctl(uint32_t request, uint64_t arg) = 0;
};
```

`ConsoleDevOps` 持一个 `ConsoleInput*`,`read`/`ioctl` 委托给它;`nullptr` 时退回 `NotImplemented`(host 测的旧行为,零回归)。真正接线在 [devfs_init.cpp](../../../kernel/fs/devfs/devfs_init.cpp):一个 `ConsoleTtyInput` 把 `read` 接到 `console_tty().read(...)`,`ioctl` 接到共享的 `console_tty_ioctl(...)`,再把这个实例注入 DevFs。

`console_tty_ioctl` 这函数是专门抽出来的——[console_tty.cpp](../../../kernel/drivers/tty/console_tty.cpp) 里一个统一的 TCGETS/TCSETS/TIOCGWINSZ/TIOCGPGRP/TIOCSPGRP/TIOCSCTTY 分派,既给 `sys_ioctl`(fd 0/1/2 走 console 的 fallback)用,也给 `/dev/console` 这个 inode 用,同一套实现:

```cpp
cinux::lib::ErrorOr<int64_t> console_tty_ioctl(uint32_t request, uint64_t arg) {
    void* uptr = reinterpret_cast<void*>(arg);
    ConsoleTty& ct = console_tty();
    switch (request) {
    case kTcgets: {
        const Termios& tm = ct.tty().termios();
        if (!cinux::user::copy_to_user(uptr, &tm, sizeof(Termios)))
            return cinux::lib::Error::Fault;   // EFAULT:坏用户指针
        return 0;
    }
    // ... TCSETS / TIOCGWINSZ / TIOCGPGRP / TIOCSPGRP / TIOCSCTTY 同理
    }
}
```

这一抽,ash 的 `isatty` 有人答了(返回 termios),read 也能拿到 cooked 行——ash 不再秒退,init 也不再瞎 respawn。

## 主线五:Error 加一个 Fault,是为了精确的 EFAULT

上面 `console_tty_ioctl` 里 `copy_to_user` 失败返的是 `Error::Fault`。这个 `Fault` 是这一章新加进 Cinux-Base 的——过去没有。

过去 `copy_from/to_user` 拿到坏用户指针,只能返 `InvalidArgument`(≈ `EINVAL`)凑合当成 `EFAULT`。**大多数地方没事**——比如 PTY 路径,没有专门验 `-EFAULT` 的测试,凑合就凑合了。**可 console 这条路上有**:`test_syscall` 里专门有 `tcgets_unmapped`(传未映射地址)、`tiocspgrp_kernel_addr`(传内核地址)这种 EFAULT 闸。你返 `-EINVAL`,闸就红了。

所以给 [expected.hpp](../../../third_party/Cinux-Base/include/cinux/expected.hpp) 的 `Error` 枚举补一个 `Fault`,语义就是"坏地址(EFAULT):用户指针被 `access_ok` 拒了或 copy 时 fault":

```cpp
enum class Error : uint32_t {
    ...
    Busy,
    Fault,  ///< Bad address (EFAULT)
};
```

`console_tty_ioctl` 返 `ErrorOr<int64_t>`,copy 失败给 `Error::Fault`;上层 `sys_ioctl` 经 `to_errno` 把 `Fault` 映射成 `-EFAULT`。精确,且保住了那两个 EFAULT 测试闸。
