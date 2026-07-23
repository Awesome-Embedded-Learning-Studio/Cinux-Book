# 批3b — busybox init 作 PID1（2026-07-02）

分支 `feat/b3b-busybox-init-pid1`（从干净 main `bf993fc`），commit `460b474` + 子模块
Cinux-Base `75df90f`。GCC 自举主线批3b：让 **busybox init 作 PID1** 按 `/etc/inittab`
respawn `/bin/sh`——批4 GCC reap 孤儿（cc1/as/ld fork 链）的硬前置。

## 目标达成（生产冒烟 build-console 非 GUI）

```
[INIT] kernel_init started tid=4 pid=1            ← 入口 alloc PID1
[EXECVE] loaded /sbin/init pid=1                  ← busybox init 以 PID1 execve
CinuxOS init: filesystems mounted                 ← inittab ::sysinit:/bin/echo
[WAITPID] reaped child pid=2 by parent pid=1      ← PID1 reap child（批4 孤儿基础）
[EXECVE] loaded /bin/sh pid=2                     ← inittab ::respawn:/bin/sh
~ #                                               ← ash 交互提示符
```

零 unhandled syscall。`/dev/tty` NotFound 是 follow-up（TIOCSCTTY 没真设 controlling_tty），
非致命——sh 仍走到 `~ #` 提示符等键盘。

## 改动（4 处 plan + 扩展 3 syscall + Cinux-Base Fault）

### 1. init.cpp — kernel_init_thread 拿 PID1（Linux kernel_init 模型）
- **⭐ 头号修正（推翻旧交接误判）**：旧说"重排 `start_poll_driver` 让 init 拿 PID1"。
  实测验伪：`task_builder.cpp:134` kthread `pid=0`（**不碰 `g_pid_alloc`**）；`g_pid_alloc`
  只在 `fork()` 分配，第一次返 1。`start_poll_driver` 的 net_poll kthread 只拿 tid，重排
  无效。**正解**：`kernel_init_thread` 入口 `g_pid_alloc.alloc()`（boot 期无人 fork，第一次
  必返 1）。execve 不动 pid，故 busybox init 继承 PID1。
- `usb::init()` 挪到 `launch_userspace()` **前**（execve 不返回，否则 usb 永远跑不到）。

### 2. shell_launch.cpp — launch_userspace 直 execve /sbin/init（不 fork）
- 删 fork→child exec /bin/sh 模式，改成 init_task 自身：`self->addr_space = new AddressSpace()`
  （execve.cpp 要求 addr_space≠null）+ argv `{"init",nullptr}`（basename 触发 busybox init
  applet）+ `launch_user_program`（execve+jump，不返回）。
- 仅改非 GUI 实现；GUI 的 desktop_launch.cpp 不动（§14 双实现，CMake 选链）。

### 3. devfs /dev/console 代理 console_tty（busybox init 前提）+ Cinux-Base Fault
busybox init `open("/dev/console")`+dup 0/1/2+setsid+TIOCSCTTY；fork 出的 ash `isatty(TCGETS)`
+read 全打 /dev/console inode ops。现状 ConsoleDevOps read 返 InvalidArgument、无 ioctl →
ash 判非交互→退出→init respawn 死循环。故：
- `devfs.hpp` 加 `ConsoleInput` 接口（read+ioctl 纯虚）；`DevFs` 构造加可选第二参（默认
  nullptr → host 测零回归：read/ioctl 返 NotImplemented，test_devfs `read is unsupported` 仍过）。
- `devfs.cpp` ConsoleDevOps 持 `ConsoleInput*`，read/ioctl 委托（nullptr→NotImplemented）。
- `devfs_init.cpp` 加 `ConsoleTtyInput`（接 console_tty + console_tty_ioctl），注入 DevFs。
- 抽 `console_tty_ioctl`（console_tty.cpp）共享：sys_ioctl fd0/1/2 fallback + devfs /dev/console
  inode 复用同一 TCGETS/TCSETS/TIOCGWINSZ/TIOCGPGRP/TIOCSPGRP/TIOCSCTTY 实现。
- **⭐ Cinux-Base Error 加 `Fault`（子模块 75df90f）**：copy_from/to_user 失败原用
  `InvalidArgument`（~EINVAL）近似 EFAULT 不准（pty_device.cpp / 旧 console_ioctl 多处
  "no Fault enum" 注释）。加 Fault + error_string + `to_errno(Fault)=EFAULT`。
  `console_tty_ioctl` 返 `ErrorOr<int64_t>`（copy 失败 `Error::Fault`）→ sys_ioctl
  `to_errno` → -EFAULT，**保住 test_syscall 的 EFAULT 闸**（tcgets_unmapped /
  tiocspgrp_kernel_addr）。devfs 路径经 sys_ioctl dispatch 也走 to_errno（Fault→EFAULT）。

### 4. create_ext2_disk.sh — /etc/inittab + /sbin/init busybox hardlink
- `/etc/inittab`（最小：`::sysinit:/bin/echo CinuxOS init: filesystems mounted` +
  `::respawn:/bin/sh`）。
- `/sbin/init` → busybox hardlink（argv[0] basename "init" → busybox init applet，
  CONFIG_INIT=y 已核；busybox_links +1 + sif）。

### 扩展：busybox init 三件套 syscall（生产冒烟暴露，plan 之外）
busybox init 以 PID1 execve 成功后卡三个未实现 syscall，逐个补：
- **vfork(58)** = fork alias。busybox init vfork /bin/sh 后立即 execve，parent 立即
  waitpid——CoW fork 语义安全（不依赖 vfork 的 shared-stack 窗口）。
- **reboot(169)** = -EPERM（CinuxOS 不支持 reboot，走 isa-debug-exit；busybox 启动期 probe
  容错）。
- **rt_sigtimedwait(128)** = **-EAGAIN**（检查 sig_pending，无则 EAGAIN）。busybox init
  主循环用它驱动 respawn（每次返回检查 ::respawn 是否需 fork）+ reap。**pure block 会
  死锁**：init 的 SIGCHLD 被 waitpid 消耗，sigtimedwait 永等 → 不检查 respawn → sh 永不
  fork。EAGAIN 让 init 循环检查；RoundRobin 时间片防饿死 sh。signal_send 加
  `Task::sigwait_blocked` 唤醒钩子（精确 opt-in，futex/waitpid 不受影响）留 future
  blocking+timeout 变体。

## 验证
- 两腿 run-kernel-test-all 绿（单核 + -smp 2，ALL TESTS PASSED 两腿，busybox 14/14 两腿）。
- host 69/69（devfs 公共接口改动零回归：DevFs 构造默认参 + ConsoleInput nullptr→NotImplemented）。
- EFAULT 测试绿（test_sys_ioctl_tcgets_unmapped_efault / tiocspgrp_kernel_addr_efault）。
- 生产冒烟（build-console 非 GUI）：busybox init PID1 → sysinit echo → respawn sh →
  ash 提示符 `~ #`。

## GOTCHA
1. **交接头号坑（重排 start_poll_driver）误判**：kthread pid=0 不碰 g_pid_alloc。正解
   init_task 入口 alloc。
2. **EFAULT 闸**：Cinux-Base Error 枚举原无 Fault，copy 失败只能 InvalidArgument（≈EINVAL）。
   console 路径有 EFAULT 测试（pty 没有，故 pty 用 InvalidArgument 没事）→ 加 Fault 根治。
3. **rt_sigtimedwait pure block 死锁**：init 的 SIGCHLD 被 waitpid 消耗，block 等不到 →
   respawn 不触发 → sh 不 fork → 永等。EAGAIN 驱动 init 循环检查 respawn。
4. **GUI vs 非 GUI**：当前 `build`（CINUX_GUI=ON）走 desktop_launch（GUI 桌面），不验
   shell_launch。批3b 生产冒烟必须 build-console（CINUX_GUI=OFF），需软链
   build/musl/busybox → build-console/musl/busybox（BUSYBOX_ELF=${CMAKE_BINARY_DIR}/musl/busybox）。
5. **kvm 现在可用**（/dev/kvm 今天 crw-rw-rw-，memory 的"无权限"已过时），QEMU 走 -accel kvm 快。
6. **测试日志重定向 /tmp**：QEMU 串口 + 编译输出上万行（含 binary ELF 段，如 `cat /hello`
   打印 ELF .symtab 字符串非踩内存），一律 `> /tmp/x.log` + `grep -an` 提取，别 cat 全文炸上下文。

## follow-up（显式不做）
- rt_sigtimedwait 真 blocking+timeout（timer_queue 驱动，替 EAGAIN 忙等）——signal_send
  sigwait 唤醒钩子已就位。
- TIOCSCTTY 真控制终端语义（controlling_tty 字段 + /dev/tty 解析；现 accept 不抢 session）。
- login/getty + /etc/passwd 真认证（用户拍板分阶段，先 init PID1→respawn sh）。
- PID1 SIGCHLD/reap 完整语义（批4 GCC 孤儿真压到再补；现已 reap 单 child）。
- 批4 GCC 工具链装盘 + `gcc hello.c`（终极目标）。
