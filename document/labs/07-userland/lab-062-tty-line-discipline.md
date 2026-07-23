---
title: Lab 062 · TTY 行规范验证
---

# Lab 062 · TTY 行规范验证

> 对应 `document/book/07-userland/062-tty-line-discipline.md`。验证档 **A 档**:这一章交付的是用户可见的能力——shell 真能交互了(退格编辑、回车提交、Ctrl+C 打断、Ctrl+D 结束输入)。所以验证的核心是真去敲键盘看效果,辅以行规范的 host 单测(纯逻辑)和内核里的机制测(Ctrl+C 真投了 SIGINT)。

## 目标

确认六件事:

1. **行规范核心是 host 可单测的纯逻辑**——`test_tty` 九个 case 罩住攒行/退格/`^C` 信号/`^D` EOF/`^U` 清行/raw 直通;
2. **termios UAPI 对齐 Linux**(布局 + c_lflag 位 + c_cc 索引),musl/glibc 拿 `TCGETS` 读出来能直接用;
3. **接键盘时 VERASE 对齐了设备字节**(`0x08` 不是默认 DEL),不然按退格没反应;
4. **stdin 是阻塞读不是忙等**(`prepare_to_wait` + `schedule_blocked`),EOF 是一次性状态;
5. **ioctl 答上 TCGETS/TCSETS/TIOCGWINSZ**,经 accessor 走、坏指针 `-EFAULT`;
6. **Ctrl+C 真投 SIGINT 给前台组**(机制测证 `sig_pending` 落了 SIGINT)。

## 步骤

### 1. 行规范纯逻辑:host 单测

```bash
./build/test/test_tty
```

应看到 `tty tests OK (9 cases)`。这九个 case 罩的是 `tty.cpp` 那块**纯逻辑**:默认 termios 校验、行积累回显、退格编辑、`^C` 产信号、`^D` 空行 EOF、`^D` 提交无换行、`^U` 清行、raw 直通、行缓冲溢出丢弃。能 host 单测是因为行规范核心写成纯函数、回显走注入 callback、信号走枚举——不碰 `kprintf`/`signal_send`。看一眼单测长什么样:

```bash
sed -n '1,30p' test/unit/test_tty.cpp
```

应看到它自己写 `int main()` 跑九个 case、注入一个 capture buffer 当 echo sink(不靠内核的 kprintf)。这就是「纯逻辑 + 注入式解耦」换来的可测性。

### 2. termios UAPI + 行规范状态机

```bash
sed -n '32,55p' kernel/drivers/tty/tty.hpp
```

应看到 `struct Termios`(iflag/oflag/cflag/lflag/c_line + `c_cc[19]`)+ `c_lflag` 位常量(`kIsig`/`kIcanon`/`kEcho`/`kEchoe`/`kEchok`)+ `c_cc` 索引(`kVintr`/`kVerase`/`kVeof`)。这些照搬 Linux `<asm-generic/termbits.h>`,布局/位值/索引都得对齐——musl 拿 `TCGETS` 读出来要能直接套进它的 `struct termios`。

再看状态机核心:

```bash
sed -n '97,142p' kernel/drivers/tty/tty.cpp
```

应看到 `input_char`:ISIG 位开时 `^C`/`^\`/`^Z` 直接产 `kSignal`(不进 `line_buf_`);ICANON 位开时换行 `commit_line` + `kLineReady`、`^D` 空行置 `eof_pending_` 返 `kEof`、`^D` 非空 `commit_line` 返 `kLineReady`(无尾换行)、VERASE 退格 + ECHOE 三连显。

### 3. 接键盘 + VERASE 设备字节错配

```bash
sed -n '30,37p' kernel/drivers/tty/console_tty.cpp
```

应看到 `init()` 里 `tm.c_cc[kVerase] = 0x08`——把默认的 DEL(`0x7F`)改成 `^H`(`0x08`),因为键盘驱动对 Backspace 键发的是 `0x08`。两个约定不一致,按退格不触发编辑。旁边还有 `set_echo_sink(&ConsoleTty::echo_via_kprintf, ...)`,把回显指向 kprintf(lock-free,IRQ 上下文能安全调)。

### 4. 阻塞读 + EOF 状态

```bash
sed -n '39,60p' kernel/drivers/tty/console_tty.cpp
```

应看到 `ConsoleTty::read`:`InterruptGuard` 关中断 → `read_cooked` 有行就返 → `take_eof` 返 0(EOF)→ `reader_ = self` + `prepare_to_wait` → 出 guard 恢复中断 → `schedule_blocked`。那个关中断的 scope 是 F3 防丢失唤醒铁律:「检查 + 登记 + 标 Blocked」原子。`take_eof` 一次性消费 `eof_pending_`,所以 EOF 只交付一次,跟「暂时没行」分开。

确认 `sys_read` 真的调它:

```bash
sed -n '61,66p' kernel/syscall/sys_read.cpp
```

应看到 `fd == 0` 走 `console_tty().read(...)`,不再有忙等循环。

### 5. ioctl 答上 musl/glibc 的探针

```bash
sed -n '69,90p' kernel/syscall/sys_ioctl.cpp
```

应看到 `switch` 分派:`kTcgets`(TCGETS,`copy_to_user` 写 termios)、`kTcsets`(TCSETS,`copy_from_user` 读 termios)、`kTiocgwinsz`(`copy_to_user` 写固定 80×25 winsize)。每条都经 `copy_to/from_user`(061 的 accessor),坏用户指针返 `-kEfault` 不 panic。fd 非 0/1/2 或未知 cmd 返 `-ENOTTY`。

### 6. Ctrl+C → SIGINT(机制测证真投了)

```bash
sed -n '384,404p' kernel/test/test_syscall.cpp
```

应看到 `test_console_tty_ctrl_c_sends_sigint_to_foreground`:造一个 pgid=5 的 Task,`set_foreground_pgid(5)`,喂 `^C`,然后断言这个 Task 的 `sig_pending` 里落了 SIGINT。这条测比「测试绿」更实——它证信号真投到了前台组。跑一下:

```bash
cmake --build build --target run-kernel-test 2>&1 | grep -iE 'ctrl_c|ctrl_z|tiocspgrp' | head
```

应看到 `test_console_tty_ctrl_c_sends_sigint_to_foreground`、`..._ctrl_z_sends_sigtstp_...`、`tiocspgrp_kernel_addr_efault` 三项 **PASS**。

再看投递那一段:

```bash
sed -n '66,93p' kernel/drivers/tty/console_tty.cpp
```

应看到 `ConsoleTty::input` 拿到 `kSignal` → `take_signal` → 映射 SIGINT/SIGQUIT/SIGTSTP → `killpg(foreground_pgid, sig)`(前台组没设回退 `reader_->pgid`)。

### 7. 真交互(punchline)

```bash
cmake --build build --target run
```

进 shell 后亲手试:

- 打一行字(比如 `echo hi`),**没按回车前**按退格——能看到字符被擦掉(ECHOE 三连显);
- 按回车——整行提交,shell 执行;
- 跑一个会循环的程序,按 **Ctrl+C**——它被打断(前台组收 SIGINT);
- 在空行按 **Ctrl+D**——shell 读到 EOF。

这一层 headless 自动测试罩不到(没真键盘),得你自己在 QEMU 里试。这是 A 档的 punchline,前面六步都是为这一刻铺的。

> 本地验证提醒:测试里有个 ring-3 musl `/hello` smoke(默认开),本地没编 `/hello` 会空转撑超时。要么先 `tools/musl/build-musl.sh` + `build-hello.sh` 编出 `/hello`,要么 `cmake -DCINUX_MUSL_HELLO_SMOKE=OFF` 关掉它(不影响行规范那些测试)。

## 验收清单

- [ ] `./build/test/test_tty` 报 9 cases OK(行规范纯逻辑 host 单测)。
- [ ] `tty.hpp:32` termios 布局对齐 Linux;`tty.cpp:97` `input_char` 状态机(ISIG 信号不进缓冲 / ICANON 攒行 / `^D` 空行 EOF 非空提交 / VERASE 退格)。
- [ ] `console_tty.cpp:34` VERASE 改 `0x08` 对齐键盘;`:36` echo sink 指 kprintf(lock-free)。
- [ ] `console_tty.cpp:39` `read` 用 `InterruptGuard` + `prepare_to_wait` + `schedule_blocked`(阻塞替忙等);`take_eof` 一次性。
- [ ] `sys_ioctl.cpp:69` TCGETS/TCSETS/TIOCGWINSZ 真命令,经 accessor 坏指针 `-EFAULT`。
- [ ] `test_console_tty_ctrl_c_sends_sigint_to_foreground` PASS;`console_tty.cpp:66` `^C` → `killpg(foreground_pgid, SIGINT)`。
- [ ] 真交互:退格编辑、回车提交、Ctrl+C 打断、Ctrl+D EOF,在 QEMU 里都灵。

## 别做这些

- **别**指望验 PTY(`/dev/ptmx`、`/dev/pts/N`、master/slave 对)——这一章只做 console TTY 单例。PTY 硬依赖设备 inode(DevFS),CinuxOS 这会儿没有,留后面。别以为「做了 TTY 就有 PTY」。
- **别**以为 winsize 是真几何——固定 80×25,因为 Console 是 `main.cpp` 局部变量、syscall 够不着。musl 要的只是「探成功了别退全缓冲」,尺寸多少不那么要紧。
- **别**以为 Ctrl+C 能打断任意程序——它投给**前台进程组**。shell 要是没 `TIOCSPGRP` 设前台组,就回退到阻塞读者(shell 自己)的组。完整 job control 要 shell 配合调 `TIOCSPGRP`。
- **别**以为单读者阻塞读在 SMP 下万事大吉——`reader_` 单读者指针单 CPU 下关中断即竞态自由,跨 CPU 无锁保护(已知 follow-up)。测试不读 console stdin 所以 `-smp 2` 不回归,但多读者 stdin 要加锁。
- **别**把行规范的「回显走 callback、信号走枚举」当成过度设计——它是 host 单测的前提。不这么解耦就得 mock `kprintf`/`signal_send`,那是另一坨坑。
