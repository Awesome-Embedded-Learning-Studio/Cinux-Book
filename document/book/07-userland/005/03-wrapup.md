---
title: 03 · 收尾:验证、没做的与小结
---

# 收尾:验证、没做的与小结

## 验证

这一章是 A 档,punchline 是用户可见的——shell 真能交互了。验证分三层。

**第一层:host 单测,行规范的纯逻辑。** `test/unit/test_tty.cpp` 九个 case:默认 termios 校验、行积累回显、退格编辑、`^C` 产信号、`^D` 空行 EOF、`^D` 提交无换行、`^U` 清行、raw 直通、行缓冲溢出丢弃。这层完全靠行规范核心那一步「纯逻辑 + 注入式解耦」的决定——能在 host 链真码,不用 mock。`ctest` 62/62。

**第二层:内核测试,机制测。** 除了既有的回归,还加了直接验信号真投的测:`test_console_tty_ctrl_c_sends_sigint_to_foreground`——造一个 Task(pgid=5),设它为前台组,喂 `^C`,查这个 Task 的 `sig_pending` 里有没有 SIGINT。还有 Ctrl+Z→SIGTSTP、TIOCSPGRP 传内核址返 `-EFAULT`。这层不只是「绿」,是证信号真投了。`run-kernel-test-all` 两 leg 各 **967 passed / 0 failed**(单核 + `-smp 2`,后者还带 AP 机制回读 PASS)。

**第三层:真交互。** 跑 `make run` 起 QEMU,进 shell,亲手敲:打一行字、按退格编辑、回车提交;跑个程序按 Ctrl+C 看它被打断;按 Ctrl+D 看 shell 读到 EOF。这一层本机的 headless 自动测试罩不到(没有真键盘输入),要靠你自己在 QEMU 里试。

> 一个本地验证的小坑得提醒:测试里有个 ring-3 的 musl `/hello` smoke(061 章那一步默认开了),它需要先用 `tools/musl/build-musl.sh` + `build-hello.sh` 编出 `/hello` 才跑;本地没编的话 smoke 会空转撑满超时。本地验证时要么先编 musl,要么 `cmake -DCINUX_MUSL_HELLO_SMOKE=OFF` 关掉它(关了不影响行规范那些测试,那些不依赖 `/hello`)。

## 这章没做的

- **PTY / `/dev/ptmx` / `/dev/pts/N`**:master/slave 对硬依赖设备 inode 这一层,CinuxOS 这会儿没 DevFS,建了是空中楼阁。这一章用 console TTY 单例绕开,行规范 + 阻塞读 + EOF + 信号都齐了,PTY 留到 DevFS(后面)落地(见 [007 · PTY](../007/))。
- **winsize 取真几何**:固定 80×25,因为 Console 是 `main.cpp` 局部变量、syscall 够不着。待 Console 全局化或 DevFS 给 fd 真设备身份。
- **shell 设前台组**:完整 job control 要 shell 在 fork 程序时主动 `TIOCSPGRP` 设前台组。现在的 shell 还没调它,所以 Ctrl+C 回退到阻塞读者(shell 自己)的组。musl/glibc 的 shell 会调;CinuxOS 自己的 shell 调不调待验。
- **这一章的改动里还顺带带了个 reap 的修复**:`-smp 2` 下子进程在别的核上退出、父进程 reap 的时序竞态(`reap_deferred` 加了 `on_cpu == -1` 同步),治一个 exit/reap 的栈 UAF #DF。它跟 TTY 没关系,是跟 TTY 同一批改动里一起落进来的,教学归到 SMP 那条线,这里不展开。

## 小结

- 之前的 stdin 是忙等轮询返 0(被 musl 当 EOF)、stdout 直接 kprintf、ioctl 全 `-ENOTTY`,交互式程序读不到真实输入。这一章补 TTY 行规范把「键盘 → 行规范 → 进程」接通。
- 行规范核心写成纯逻辑(回显走注入 callback、信号走枚举),换来 host 链真码单测;ICANON 攒行/退格/回车提交、ISIG 把 `^C`/`^\`/`^Z` 翻译成信号不进缓冲。
- 接键盘时两个坑:回显 sink 必须 lock-free(IRQ 上下文不能拿锁打印);设备层字节(`^H`)跟 UAPI 默认值(DEL)对不上,初始化时显式对齐 VERASE。
- 阻塞读用 `prepare_to_wait` + `schedule_blocked` 替忙等,关中断下「检查 + 登记 + 标 Blocked」原子防丢失唤醒;EOF 当状态(`eof_pending_` + `take_eof` 一次性),跟「暂时没行」分开。
- ioctl 接成真命令(TCGETS/TCSETS/TIOCGWINSZ),经 061 的 accessor 走、坏指针 `-EFAULT`;Ctrl+C 经 `killpg` 投前台组,终端能打断程序了。
