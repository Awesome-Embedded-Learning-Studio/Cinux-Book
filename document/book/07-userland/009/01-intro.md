---
title: 01 · busybox 当 PID1:init 不是 fork 出来的
---

# busybox 当 PID1:init 不是 fork 出来的

> 073 把静态 busybox 跑起来了,可它是在测试内核里被一次性 `execve` 起来的——跑完就退。一个真系统不是这样的:**PID 1 永远活着,它是所有孤儿进程的归宿,是 `init`**。这一章的 punchline 是:**让 busybox 的 `init` applet 当 PID 1**,按 `/etc/inittab` respawn `/bin/sh`,顺带把"PID 1 为什么永远是 1"这件最基础的 Unix 事讲透。
>
> 这章有两个反直觉的坑值得专程来一趟。第一个:**PID 1 不是 `fork` 出来的**——内核 init 线程在入口处直接从 PID 分配器里领走 1 号,`execve` 又不换 pid,所以 busybox init 天然继承 PID 1。第二个:**`rt_sigtimedwait` 不能真阻塞**——busybox init 的主循环靠它的返回值驱动 respawn,一旦真睡死,`/bin/sh` 永远不会被 fork 出来,整个系统死锁。
>
> A 档:punchline 是非 GUI 构建启动后,串口能看到 `[INIT] pid=1` → busybox init 跑 `/etc/inittab` 的 sysinit → respawn 出 `/bin/sh` → ash 的 `~ #` 提示符。机制测试绿 ≠ init 真能跑起来——这一章的验收必须走非 GUI 的生产启动。

## 这章咱们要点亮什么

1. **PID 1 不是 fork 出来的**:内核线程天然 `pid=0`(它压根不碰 PID 分配器),PID 分配器只在 `fork()` 里发号,boot 期又没人 fork 过,所以第一次 `alloc()` 必返 1。init 线程在入口领走 1 号,这就是"init 永远是 1"的真正机制——不是什么魔法,是分配器语义。
2. **`execve` 保 pid**:换的是程序映像,不换 pid。所以 init 线程领了 PID 1 之后 `execve /sbin/init`,busybox init 接过这个 1 号身份,名正言顺地 reap 孤儿。
3. **`rt_sigtimedwait` 真阻塞会死锁**:busybox init 的主循环用 `rt_sigtimedwait` 等信号,靠它的**返回**去检查"该不该 respawn sh、该不该 reap child"。如果让它纯阻塞等信号,而此时还没有任何 child 被 fork 出来(也就没有 SIGCHLD),它就永等 → 不检查 respawn → sh 永远不 fork → 死锁。正解是没信号就返 `-EAGAIN`,逼 init 转起来。
4. **`/dev/console` 得会读、会回 `ioctl`**:busybox init 一上来 `open("/dev/console")` + dup 0/1/2 + `setsid` + `TIOCSCTTY`;它 fork 出的 ash 要 `isatty`(发 `TCGETS`)、要读行。若 `/dev/console` 的 read 返回错、`ioctl` 不答,ash 判定"我不是交互终端"→ 当场退出 → init 又 respawn → 死循环。所以得给 `/dev/console` 接上真 console TTY 的后端。
5. **`Error::Fault` 不是为了好看**:`copy_from/to_user` 拿到坏指针,过去只能返 `InvalidArgument`(≈ `EINVAL`)凑合当 `EFAULT`。可 console 这条路上有专门验 `-EFAULT` 的测试,凑合就过不了闸——给 `Error` 枚举补一个 `Fault`,精确映射 `EFAULT`。
