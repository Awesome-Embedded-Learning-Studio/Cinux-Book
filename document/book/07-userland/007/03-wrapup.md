---
title: 03 · 收尾:验证、没做的与小结
---

# 收尾:验证、没做的与小结

## 验证

四层验证。

**第一层:host 单测,PTY 纯逻辑。** `test/unit/test_pty.cpp` 一组 case(上面列过):canonical round-trip、本地回显、slave 输出 → master、`^C` 信号、raw、退格、`^D` EOF、满环 partial、双实例不串扰。`./build/test/test_pty` 全过。

**第二层:内核测试,PTY 设备 + 控制终端。** `kernel/test/test_pty_device.cpp` 七例:alloc/slave-lookup、master↔slave canonical round-trip、slave 输出 → master、本地回显 → master、slave→Pty termios ICANON 接线(白盒)、未知 ioctl 拒绝、release 复用。再加 `TIOCSCTTY` 的 session leader / 非-leader 两例(负测:非 leader → EACCES)。

**第三层:回归网。** 改公共头 `inode.hpp`(加 ioctl/open virtual)是大事,得有回归网罩:`test_sys_ioctl_non_tty_fd_enotty`(fd 99 无 File → -ENOTTY)、`test_sys_ioctl_unknown_cmd_enotty`、console 的 TCGETS/TCSETS/TIOCGPGRP/TIOCSPGRP 全过——证 fd≤2 行为零变、fd>2 默认路径仍 -ENOTTY。

**第四层:boot 冒烟。** `make run` 起 QEMU,看 `[DEVFS] mounted at /dev (4 nodes)`——比 064 多了一个 `ptmx` 节点。生产 boot 真加载了 `/dev/ptmx` + `/dev/tty` + `/dev/pts/N` resolver,零 panic。

`run-kernel-test-all` 单核和 `-smp 2` 两条腿都过(在既有基线上加了 9 个 PTY 设备/控制终端测)。

## 这章没做的

- **fork+execve-under-PTY 全闭环**:真用户程序跑在 PTY 里要 `sys_dup2`(把 slave 重定向成子进程 stdio——**当前未实现**)+ 真 session + 一个会 `open("/dev/ptmx")` 的 shell。这一章没合成 ring3 PTY smoke,留 CFBox 那条线(它本就需要 dup2,是 PTY 的天然消费者)。另:ring0 测试 harness 不能调带 user 指针的 syscall(`access_ok` 拒内核址),所以纯 ring0 syscall smoke 也做不了——验证靠内核侧白盒测 + host 纯逻辑测。
- **blocking slave read**:现在 slave read 非阻塞(0 = 无数据,EOF 经 `take_eof` 分)。真 shell 读 stdin 要阻塞(像 console_tty 那样睡等行),要 reader Task + 唤醒机制,留 follow-up。
- **PTY close → release**:close master/slave fd 时该 `pty_release(index)` 释放槽位。现在没 close 钩子(槽位靠测试显式 release),生产用要补。
- **errno 近似**(Cinux-Base 的 Error 枚举所限,子模块边界不擅改):copy 失败 → EINVAL(Linux 会 EFAULT);TIOCSCTTY 拒绝 → EACCES(Linux EPERM)。行为对,errno 近似。
- **PTY 限 8 对**(`kMaxPtys`);动态扩展留后。

## 小结

- PTY 是一对 master/slave:slave 对程序像真终端(行规范 + termios + 信号),master 是模拟器端。开一对 = 开一个新终端,把 062 的 console 单例升级成多路。
- PTY 核心(`Pty` 类)是纯逻辑,slave 复用 062 的 `TTY` 行规范(不重写),回显经 echo sink 路由回 master 读侧;四条数据路径 host 单测全过。
- 两条新接缝让 PTY 接进 fd:`InodeOps::ioctl`/`open` virtual(对齐 Linux fops),`sys_ioctl`/`sys_open` 对 fd>2 走 `fd→File→Inode→ops` 派发(fd≤2 console 零变,NotImplemented→ENOTTY)。
- `/dev/ptmx` 克隆 open(`PtmxOps::open` 分配一对返 master)+ `/dev/pts/N` DevFS 动态查找;8 槽固定注册表,inode 号编码 pty 索引,reset() 避 echo sink 悬垂。
- `TIOCSCTTY` 挂控制终端 + `/dev/tty` 每进程别名,PTY 有完整 session/前台组/信号语义。
- 全闭环(真程序跑 PTY 里)留 CFBox——要 dup2 + 真 shell;blocking read、close→release、errno 近似、8 槽上限是已知 follow-up。
