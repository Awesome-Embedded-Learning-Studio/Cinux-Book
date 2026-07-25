---
title: Lab 009 · busybox 当 PID1:init 不是 fork 出来的
---

# Lab 009 · busybox 当 PID1:init 不是 fork 出来的

> 对应 `document/book/07-userland/009/`。验证档 **A 档**:punchline 是非 GUI 构建启动后,串口看到 `[INIT] pid=1` → busybox init 跑 `/etc/inittab` → respawn 出 `/bin/sh` → ash 提示符 `~ #`。init PID1 这件事机制测试证明不了(非 GUI 的 `shell_launch` 测试内核不走),必须走非 GUI 生产启动;没编 busybox 的话,走步骤 1、2 的 grep 锚点 + build-console 编译验内核侧。

## 目标

确认五件事:

1. **PID1 是 init 线程入口 `alloc()` 领来的**(不是 fork 出来的);
2. **`execve` 保 pid**:init 线程领 1 号后直接 `execve /sbin/init`,busybox init 继承 PID1;
3. **`rt_sigtimedwait` 返 `-EAGAIN` 而非阻塞**(纯阻塞会死锁);
4. **`/dev/console` 接了真 console TTY 后端**(ash 的 `isatty`/read 有人答,不再秒退);
5. **`Error::Fault` 精确映射 EFAULT**(console 路径的 EFAULT 测试闸过)。

## 步骤

### 1. 非 GUI 构建编得过(shell_launch /sbin/init 路径)

GUI 构建链的是 `desktop_launch.cpp`,**不编** `shell_launch.cpp`。所以光测 GUI 构建绿,证明不了 `/sbin/init` 这条路编得过。得单开一份非 GUI 构建:

```bash
cmake -S . -B build-console -DCINUX_GUI=OFF -DCINUX_USB=ON
cmake --build build-console -j$(nproc) 2>&1 | grep -iE 'shell_launch|error' | head
```

应看到一行 `Building CXX object .../proc/shell_launch.cpp.o`,且没有 error。这说明 §14 的非 GUI 那份 `launch_userspace()`(execve `/sbin/init`)编进去了。

> 验完后可以 `rm -rf build-console`,它只是为这一步验编译,不影响主 `build`。

### 2. grep 锚点:四条主线的代码落地

每条主线在源码里都能 grep 到。逐条核对:

```bash
# (a) PID1 是入口 alloc 领的,不是 fork
grep -n 'g_pid_alloc.alloc' kernel/proc/init.cpp
# 应看到 self->pid = g_pid_alloc.alloc(); 上方注释讲"kthread pid=0、boot 期无人 fork、第一次返 1"

# (b) shell_launch 直 execve /sbin/init,不 fork
grep -n 'sbin/init\|launch_user_program\|argv\[\]' kernel/proc/shell_launch.cpp
# path="/sbin/init"、argv={"init",nullptr}、launch_user_program(...)

# (c) rt_sigtimedwait 没信号返 -EAGAIN
grep -n 'EAGAIN\|sig_pending & wait_set\|sigwait_blocked' kernel/syscall/sys_signal.cpp kernel/proc/signal.cpp kernel/proc/process.hpp
# sys_signal.cpp 注释讲"pure block 会死锁";process.hpp 有 sigwait_blocked 钩子字段

# (d) /dev/console 接真 console TTY(注入,不破坏 host 测)
grep -n 'class ConsoleInput\|ConsoleTtyInput\|console_tty_ioctl' kernel/fs/devfs/devfs.hpp kernel/fs/devfs/devfs_init.cpp kernel/drivers/tty/console_tty.cpp
# devfs.hpp 抽 ConsoleInput 纯接口;devfs_init.cpp 注入 ConsoleTtyInput;console_tty.cpp 抽共享 console_tty_ioctl

# (e) Error::Fault(EFAULT)
grep -n 'Fault' third_party/Cinux-Base/include/cinux/expected.hpp kernel/drivers/tty/console_tty.cpp
# 枚举加 Fault;console_tty_ioctl copy 失败返 Error::Fault
```

### 3. 机制测试无回归

```bash
cmake --build build --target run-kernel-test 2>&1 | tail -5
```

应看到 `Tests: <N> passed, 0 failed` + `ALL TESTS PASSED`。本章动了 console_tty / devfs / signal / fork / init,PID1 alloc + usb 前移不该让任何机制测红。

### 4.(A 档 punchline)非 GUI 生产启动:看 pid=1 + respawn sh

> 这一步要 busybox。先照 Lab 008 把 musl + busybox 编出来(如果 `build/musl/busybox` 已在,跳过):
> ```bash
> tools/musl/build-musl.sh
> tools/musl/build-busybox.sh
> ```
> busybox 装盘的 `/sbin/init` 硬链接 + `/etc/inittab` 由 `create_ext2_disk.sh` 自动加(BUSYBOX_ELF 第 7 参命中时)。

非 GUI 构建启动 init PID1,需要把 busybox 指给 build-console(BUSYBOX_ELF 走 `${CMAKE_BINARY_DIR}/musl/busybox`,build-console 里没有,软链一下主 build 的):

```bash
mkdir -p build-console/musl
ln -sf "$(pwd)/build/musl/busybox" build-console/musl/busybox
cmake --build build-console --target run 2>&1 | tee /tmp/init_boot.log
```

串口上按顺序应看到:

```
[INIT] kernel_init started tid=4 pid=1            ← 入口 alloc 领到 PID1
[INIT] Executing /sbin/init as PID1 (busybox init)...
[EXECVE] loaded /sbin/init pid=1                  ← busybox init 以 PID1 execve(保 pid)
CinuxOS init: filesystems mounted                 ← /etc/inittab 的 ::sysinit
[EXECVE] loaded /bin/sh pid=2                     ← ::respawn:/bin/sh
~ #                                               ← ash 交互提示符
```

`pid=1` 这一行是整章的眼。`/dev/tty` NotFound 是已知非致命边界(TIOCSCTTY 没真设控制终端),ash 照样走到 `~ #` 等键盘。

> 没编 busybox 的话,这道跳过——步骤 1、2、3 已经把内核侧验透了,这一步是"真程序跑起来"的端到端确认。

## 想一想

1. **为什么 init 线程要在入口 `alloc()`,而不是像旧实现那样 fork 出 shell?** 提示:fork 出来的 shell 是 PID 2,它和"init respawn 我"是什么关系?谁该是 PID 1?
2. **把 `rt_sigtimedwait` 改成纯阻塞(没信号就睡死)会怎样?** 提示:busybox init 还没 fork 任何 child 时,有没有 SIGCHLD?它靠什么返回值驱动 respawn?
3. **`usb::init()` 为什么得挪到 `launch_userspace()` 前面?** 提示:非 GUI 的 `launch_userspace` 内部 `execve` + 跳用户态,它返回吗?
4. **为什么 `/dev/console` 的 console 后端要走 `ConsoleInput` 接口注入,而不是直接在 `devfs.cpp` 里 `#include console_tty`?** 提示:064 章给 DevFS 立过的那条"host 可测"栅栏。
