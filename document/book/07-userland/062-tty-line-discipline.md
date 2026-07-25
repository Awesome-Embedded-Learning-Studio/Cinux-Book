---
title: 062 · TTY 行规范 —— 让 shell 真正能交互(退格、回车、Ctrl+C)
---

# 062 · TTY 行规范 —— 让 shell 真正能交互(退格、回车、Ctrl+C)

> 上一章(059)让内核跑起了用真 musl 编译的静态程序,`hello` 能打出 `Hello from musl` 干净退出了。可一旦你想跟它**交互**——比如 `scanf` 读你敲的输入——就读不到。因为 stdin(`fd==0`)的实现是键盘 PS/2 环形缓冲上的**忙等轮询**:没键就空转一百万次,然后返回 0;而 musl 把「read 返回 0」当成 **EOF**,于是交互式程序一上来就以为输入结束了。stdout 那边也没好到哪去:`fd==1` 直接走 `kprintf`,没有行缓冲;`ioctl` 不管问什么都返回 `-ENOTTY`,musl 探窗口大小(TIOCGWINSZ)失败就退回全缓冲,`printf` 的输出不及时。键盘敲一个字符就交给程序一个字符,没有退格编辑、没有回车成行提交、没有 Ctrl+C——离一个真终端差远了。
>
> 这一章补一个真的 **TTY 子系统**,把「键盘 → 行规范 → 进程」这条链接通。行规范(line discipline)是终端的灵魂:它在原始按键和程序看到的输入之间加一层加工——攒一行、处理退格、回车才提交、把 Ctrl+C 翻译成信号。然后接上阻塞读(让 shell 等 stdin 时 CPU 不空转)、接上真 ioctl(答上 musl/glibc 的探针)、把 Ctrl+C 接成真的 SIGINT 投给前台进程组。做完这些,shell 就是个能用的交互终端了:A 档的 punchline 就是这个——敲退格能编辑、回车提交整行、Ctrl+C 打断前台、Ctrl+D 结束输入。
>
> 一条诚实的边界先说在前头:这一章做的是 **console TTY**(系统唯一一个终端,单例,键盘当输入、串口/Console 当回显),**不是 PTY**。PTY(master/slave 对、`/dev/ptmx`、`/dev/pts/N`)要建设备 inode,而 CinuxOS 这会儿还没有 DevFS,建了也是空中楼阁——PTY 留到后面 DevFS 落了再做。console TTY 单例绕开设备节点这一层,功能(行规范 + 阻塞读 + EOF + 信号)是完整的,不欠债。

## 这章咱们要点亮什么

1. **行规范是原始按键和程序输入之间的一层加工**:ICANON 模式下攒一行、退格编辑、回车或 `^D` 才提交;ISIG 模式下把 `^C`/`^\`/`^Z` 翻译成信号,这些字符根本不进输入缓冲。
2. **怎么让一块内核逻辑能在 host 上单测**:行规范核心写成纯逻辑,回显走注入的 callback、信号走枚举——不直接碰 `kprintf`、不直接碰 `signal_send`,于是 host 能链真码跑单测,而不是手搓 mock。
3. **阻塞读的两件难事**:一是别让 CPU 空转(用 `prepare_to_wait` + `schedule_blocked` 替忙等);二是「read 返回 0 = EOF」和「read 暂时没数据 = 该阻塞」这两个语义得分开,不然 `^D` 会让 read 一直返 0 假 EOF。
4. **ioctl 不再是摆设**:`TCGETS`/`TCSETS` 读写 termios、`TIOCGWINSZ` 答窗口尺寸,解锁 musl/glibc 的行缓冲;而且这些经 061 那套 accessor 走,坏用户指针返回 `-EFAULT` 而不是 panic。
5. **Ctrl+C 接成真信号**:`^C` 在行规范里被认出来,映射成 SIGINT,经 `killpg` 投给前台进程组——终端能打断程序了。

## 先看清现状:stdin/stdout 有多简陋

动手之前,先 grep 坐实一下之前的家底有多薄。

stdin(`fd==0`)之前是键盘 PS/2 环形缓冲上的忙等轮询——没键就空转,转完返 0,被 musl 当 EOF。所以一个想读你输入的程序,一上来就 EOF 结束了。stdout(`fd==1`)直接 `kprintf`,一个字节吐一个字节,没有「攒一行再 flush」这回事。`ioctl` 更干脆,不管什么 request 都返 `-ENOTTY`——可 musl/glibc 启动时会拿 stdout 探 `TIOCGWINSZ`(终端窗口多大),探失败就退回全缓冲,你 `printf` 的东西要等缓冲区满才看得见。

地基倒是大多现成:F3 的信号、进程组、`killpg` 已经做完;F3-M3 还预埋了一个 `Task::controlling_tty` 字段(没终端时是 -1);Console 已经能当 echo 的 sink。差的就差一个**真 TTY 子系统**,把键盘 → 行规范 → 进程这条链接起来。

## 行规范:一块能在 host 上单测的纯逻辑

第一步只做**纯逻辑**,新建 `kernel/drivers/tty/tty.{hpp,cpp}`,不接任何内核依赖——不碰 `sys_read`、不碰键盘、不碰 Console。目的是这块代码能直接在 host 上链真码跑单测。

先把 UAPI 立起来。`tty.hpp` 照搬 Linux `<asm-generic/termbits.h>` 的 `struct Termios`(iflag/oflag/cflag/lflag/c_line + `c_cc[19]`),加上 `c_lflag` 的位常量和 `c_cc` 索引:

```cpp
constexpr uint32_t kIsig   = 0b0000001;  // ISIG  (signal chars generate signals)
constexpr uint32_t kIcanon = 0b0000010;  // ICANON (canonical / line mode)
constexpr uint32_t kEcho   = 0b0001000;  // ECHO
constexpr uint32_t kEchoe  = 0b0010000;  // ECHOE (erase: bs space bs)
...
constexpr uint8_t kVintr   = 0;   // ^C 的 c_cc 索引
constexpr uint8_t kVerase  = 2;   // 退格
constexpr uint8_t kVeof    = 4;   // ^D
```

(`tty.hpp:42`、`:49`。)对齐 Linux UAPI 不是洁癖——musl/glibc 拿 `TCGETS` 读出来的 termios 要能直接套进它们的数据结构,布局错一位全完蛋。

> 这一批最关键的一个设计决定:**回显和信号都解耦**。`TTY` 类自己**不直接**调 `kprintf`(那是内核的东西)、也**不直接**调 `signal_send`(那是进程子系统的东西)。回显走**注入的 callback**:`set_echo_sink(emit, ctx)`;信号走**枚举**:`TtySignal`(kNone/kSigint/kSigquit/kSigtstp),行规范检测到 `^C` 只是记下「有个 SIGINT 待处理」,至于谁来投、怎么投,是上层的事。
>
> 代价是上层(console_tty)要写一层胶水接线。换来的好处是决定性的:`tty.cpp` 成了纯函数,host 测试框架能直接链它跑真码——退格编辑、`^C` 产信号、`^D` EOF、`^U` 清行、raw 模式直通,九个 case 一字不差地验。要是不解耦,就得在 host 里 mock 掉 `kprintf` 和 `signal_send`,那是另一坨坑。这个「纯逻辑 + 注入式解耦」的套路,以后接键盘、终端类逻辑都该照。

行规范状态机的核心是 `input_char`,ICANON 下逐字节喂:

```cpp
InputResult TTY::input_char(char c) {
    if (termios_.c_lflag & kIsig) {           // ISIG:^C/^\^Z 直接产信号,不进缓冲
        if ((uint8_t)c == termios_.c_cc[kVintr]) { pending_signal_ = kSigint;  return kSignal; }
        if ((uint8_t)c == termios_.c_cc[kVquit]) { pending_signal_ = kSigquit; return kSignal; }
        if ((uint8_t)c == termios_.c_cc[kVsusp]) { pending_signal_ = kSigtstp; return kSignal; }
    }
    if (termios_.c_lflag & kIcanon) {         // ICANON:攒一行
        if (c == '\n') { commit_line(); ...; return kLineReady; }   // 回车提交整行
        if ((uint8_t)c == termios_.c_cc[kVeof]) {                    // ^D
            if (line_len_ == 0) { eof_pending_ = true; return kEof; } // 空行 → EOF
            commit_line(); return kLineReady;                         // 非空 → 提交(无尾换行)
        }
        if ((uint8_t)c == termios_.c_cc[kVerase]) {                   // 退格
            if (line_len_ > 0) { line_len_--; /* ECHOE: \b 空格 \b 三连显 */ }
            return kConsumed;
        }
        ...
    }
}
```

(`tty.cpp:80`。)几条规则读着自然,但每条都有讲究:`^C` 在 ISIG 开时**根本不进 `line_buf_`**,直接产信号(不然你打断程序的那下 `^C` 会混进输入);`^D` 在空行才是 EOF,在非空行是「提交已缓冲内容但不带尾随换行」(这是 `^D` 提交的语义,跟回车不同);退格走 ECHOE 三连显 `\b` `空格` `\b`(退一格、用空格抹掉原字符、再退一格),光标才正确回到原位。非 ICANON(raw)模式则每字节直通,不做任何加工。

## 接上键盘和回显

纯逻辑就位,第二步把它接到真实的键盘 IRQ。新建 `kernel/drivers/tty/console_tty.{hpp,cpp}`:系统一个 **console TTY 单例**,键盘当输入源,回显走和 stdout 同一个 sink。

接线有两处,各有一个坑值得记。

**坑一:回显 sink 必须是 lock-free 的。** 注入的 echo sink 指向 `kprintf`。为什么是 `kprintf` 不能是别的?因为键盘回调跑在 **IRQ 上下文**,在 IRQ 里调一个拿锁的打印会死锁(锁的持有者可能正被这个 IRQ 打断)。`kprintf` 是逐字符走 `Serial::putc`、无锁的,正好能用——代价最多是并发时偶发一个瞬时光标错位,不致命。

**坑二:设备层发的字节,跟 UAPI 默认值对不上。** termios 默认的 VERASE(退格键)是 DEL(`0x7F`),可键盘驱动对 Backspace 键发的是 `^H`(`0x08`)。两个约定不一致,你按退格根本不触发编辑——行规范等的是 DEL,等不到。`console_tty_init()` 里显式把 VERASE 改成 `0x08` 对齐键盘:

> 这种「UAPI 默认值 vs 实际硬件字节」的错配,不主动接缝就踩。默认值是标准定的,硬件发什么是驱动定的,两边没人为对方负责,接缝处(初始化时)得有人显式对齐。这种坑不会报错,只会「按了没反应」,debug 起来特别费劲——记住这个模式:设备接进来时,主动核对它发的字节跟 UAPI 默认值吻不吻合。

键盘那边,`dispatch_key` 在「按键按下 + ascii 非零」时喂一个字节给行规范(`kernel/drivers/keyboard/keyboard.cpp:328`),回车 `\r` 转成 `\n`(`keyboard.cpp:327`)。这一步行规范还没接阻塞读——`sys_read` 对 `fd==0` 还是先留着忙等兜底,一步一步接,先把「键盘 → 行规范 → 回显」这条链跑通。

## 阻塞读:CPU 不再空转,EOF 是状态不是事件

第三步干掉那个忙等,真正让 shell 等 stdin 时 CPU 不空转——同时把 `^D` 的 EOF 接通。这是这一步的核心价值,顺手修掉「musl 把忙等超时的返回 0 误当 EOF」那个硬伤。

先说 EOF。`^D` 在空行该让 read 返回 0(EOF),可「暂时没行」时 read 也返回 0(意思是「该阻塞」)——两个语义撞在同一个返回值上。要是不分开,`^D` 按一下,read 返 0,以后每次 read 都返 0(假 EOF),程序直接结束。解法是把 EOF 当**状态**而不是事件:一个 `eof_pending_` 标志,`take_eof()` 消费一次就清零——保证 **EOF 只交付一次**,跟「暂时没行」泾渭分明。

再说阻塞读。`ConsoleTty::read` 长这样:

```cpp
size_t ConsoleTty::read(char* buf, size_t len) {
    auto* self = Scheduler::current();
    for (;;) {
        {
            InterruptGuard guard;                   // 关中断
            size_t n = tty_.read_cooked(buf, len);  // 有行?直接返
            if (n > 0) return n;
            if (tty_.take_eof()) return 0;          // ^D 空行 → EOF
            reader_ = self;
            Scheduler::prepare_to_wait(self);       // 标记将要阻塞
        }                                            // 出 guard 前恢复中断
        Scheduler::schedule_blocked();              // 切走,被唤醒后循环再读
    }
}
```

(`console_tty.cpp:41`。)`do_read_kernel`(kernel-to-kernel 那一层)对 `fd==0` 调它(`sys_read.cpp:91`),忙等删掉,CPU 不再空转。P0b SMAP 分层后,`fd==0` 的阻塞读在 `do_read_kernel` 里写 kernel staging buffer,`sys_read` 只负责把字节 `copy_to_user`——阻塞不跨 `stac` 窗口。

> 这里那个 `InterruptGuard` + `prepare_to_wait` 的顺序是 F3 立的防丢失唤醒铁律。「检查有没有行、登记自己是读者、标记 Blocked」这三步必须在**关中断下原子完成**。不然有个要命的窗口:检查时没行 → 还没登记自己 → 键盘正好来了行 → feeder 找不到读者(还没登记)→ 我登记完了睡下 → 唤醒永远不来。关中断把这三步缝死,feeder 要么在我检查之前来(我看到行),要么在我睡下之后来(它叫得醒我),没有中间态。这套 `prepare_to_wait`/`schedule_blocked` 是 CinuxOS 已验证的标准缝,pipe、waitpid 都用它。
>
> 一个 SMP 的边界得诚实交代:`reader_` 是单读者指针(假设 shell 是 stdin 唯一的读者),**单 CPU 下关中断就是竞态自由**;可跨 CPU 时,两个核同时动 `reader_` 没有锁保护。测试不读 console stdin,所以 `-smp 2` 下不回归,但这是个已知的 follow-up——真要支持多读者 stdin,得加 Mutex 风格的 spinlock。console TTY 单读者的假设在当前(一个 shell)下成立,记着这层。

## ioctl:答上 musl/glibc 的探针

行规范通了,可 `ioctl` 还是那个返 `-ENOTTY` 的桩。问题在:musl/glibc 一写 stdout 就拿它探 `TIOCGWINSZ`(终端多大),探失败退回全缓冲,`printf` 攒够一桶才 flush。`TCGETS`/`TCSETS`(读写 termios)也是 raw 模式、信号字符配置的前置。第四步把这些接成真命令。

per-request 逻辑住在一个共享的 `console_tty_ioctl`(`console_tty.cpp:148`,一个 `switch(request)` 分派);`sys_ioctl` 本身只做 fd-table 分派——先查已安装的 File 走它的 inode->ops->ioctl(这样 GUI shell 的 PTY slave fds 才不会误走 console),没 fd-table 项的 legacy 0/1/2 才 fallback 到 `console_tty_ioctl`:

- **TCGETS / TCSETS**:读写 console TTY 的 termios(`ct.tty().termios()` / `set_termios()`)。
- **TIOCGWINSZ**:返窗口尺寸,固定 80×25。
- 只有 `fd` 0/1/2(stdin/stdout/stderr,都背靠 console TTY)才答;别的 fd 返 `-ENOTTY`;未知 cmd 也 `-ENOTTY`。

每一命令的用户缓冲访问都走 061 那套 `copy_to_user`/`copy_from_user`(带 exception table 的 accessor):

```cpp
case kTcgets: {
    const Termios& tm = ct.tty().termios();                 // 直接取 const&,不是 fill 进传入的 tm
    if (!copy_to_user(uptr, &tm, sizeof(Termios))) return cinux::lib::Error::Fault;  // 坏指针 → EFAULT 不 panic
    return 0;
}
```

> 这里把返回值压成 `Error::Fault` 为可读性——`console_tty_ioctl` 本身返回 `ErrorOr<int64_t>`,坏用户指针写的就是 `cinux::lib::Error::Fault`(`console_tty.cpp:152`);到了 `sys_ioctl` 的 fallback adapter 才 `to_errno` 成 `-EFAULT`。

(`console_tty.cpp:148`。)这里正好用上 061 的成果——用户传个未映射的地址进来,accessor 的 `rep movsb` fault,exception table 拦下,返 `-EFAULT`,而不是把内核炸了。测试里专门有一条拿 `0x7000000000`(那个落用户半区但谁都没映射过的地址)探 TCGETS,期望就是 `-EFAULT`。

> 为什么 winsize 固定 80×25 不取真几何?因为 Console 现在是 `main.cpp` 里的局部变量,syscall 层够不着它,拿不到 framebuffer 的真实尺寸。这是个待解的结(全局化 Console,或者等 DevFS 给 fd 一个真设备身份),暂时固定 80×25 够用——musl 要的只是「探成功了、别退全缓冲」,尺寸是多少不那么要紧。

## Ctrl+C 接成真信号

最后一步把 `^C`/`^\`/`^Z` 接成真信号投递。之前 `console_tty_input` 只在 `kLineReady`/`kEof` 时唤醒读者,**不处理 `kSignal`**——所以行规范已经认出了 `^C`(记了 `pending_signal_`),却没人来取,按 Ctrl+C 没反应。第五步补上这条线。

`ConsoleTty::input` 拿到 `kSignal` 后(`console_tty.cpp:93`):先 `take_signal()` 消费掉(对齐 `take_eof`,一次性),映射成 POSIX 信号(interrupt→SIGINT / quit→SIGQUIT / suspend→SIGTSTP),再 `killpg(foreground_pgid, sig)` 投给前台进程组。前台组没设(`==0`)就回退到阻塞读者的组(shell)。

```cpp
if (r == InputResult::kSignal) {
    TtySignal ts = tty_.take_signal();
    Signal sig = ...;  // kSigint / kSigquit / kSigtstp
    int pgid = foreground_pgid_;
    if (pgid == 0 && reader_ != nullptr) pgid = reader_->pgid;  // 没前台组 → 回退读者组
    if (pgid != 0) killpg(pgid, sig);
    return;
}
```

(`console_tty.cpp:90`。)`killpg` 内部的注册表锁用 `irq_guard`(IRQ-safe),所以从键盘 IRQ 上下文调它安全——嵌套 `cli` 无害。配套的 `TIOCGPGRP`/`TIOCSPGRP` 也在 `console_tty_ioctl` 里读写 `foreground_pgid`,让 shell 能设前台组(完整 job control 的一环)。

> 这一步还顺带把 `console_tty` 从 C 风格收进了 `ConsoleTty` 类。之前它是全局 `static` 变量 + 一堆自由函数;行规范那几批还能凑合,到加 `foreground_pgid` 这种「跨方法共享的可变状态」时,全局 static 就开始别扭了。判断信号很简单:全局 static 里有**超过一个可变字段、且跨多个自由函数共享**,就该类化。(对比键盘/鼠标驱动,它们是无状态/单值工具,全 static 合理。)收进类之后,`reader_`/`foreground_pgid_`/`tty_` 都成了成员,caller 从自由函数调用改成方法调用。

## 验证

这一章是 A 档,punchline 是用户可见的——shell 真能交互了。验证分三层。

**第一层:host 单测,行规范的纯逻辑。** `test/unit/test_tty.cpp` 九个 case:默认 termios 校验、行积累回显、退格编辑、`^C` 产信号、`^D` 空行 EOF、`^D` 提交无换行、`^U` 清行、raw 直通、行缓冲溢出丢弃。这层完全靠行规范核心那一步「纯逻辑 + 注入式解耦」的决定——能在 host 链真码,不用 mock。`ctest` 62/62。

**第二层:内核测试,机制测。** 除了既有的回归,还加了直接验信号真投的测:`test_console_tty_ctrl_c_sends_sigint_to_foreground`——造一个 Task(pgid=5),设它为前台组,喂 `^C`,查这个 Task 的 `sig_pending` 里有没有 SIGINT。还有 Ctrl+Z→SIGTSTP、TIOCSPGRP 传内核址返 `-EFAULT`。这层不只是「绿」,是证信号真投了。`run-kernel-test-all` 两 leg 各 **967 passed / 0 failed**(单核 + `-smp 2`,后者还带 AP 机制回读 PASS)。

**第三层:真交互。** 跑 `make run` 起 QEMU,进 shell,亲手敲:打一行字、按退格编辑、回车提交;跑个程序按 Ctrl+C 看它被打断;按 Ctrl+D 看 shell 读到 EOF。这一层本机的 headless 自动测试罩不到(没有真键盘输入),要靠你自己在 QEMU 里试。

> 一个本地验证的小坑得提醒:测试里有个 ring-3 的 musl `/hello` smoke(061 章那一步默认开了),它需要先用 `tools/musl/build-musl.sh` + `build-hello.sh` 编出 `/hello` 才跑;本地没编的话 smoke 会空转撑满超时。本地验证时要么先编 musl,要么 `cmake -DCINUX_MUSL_HELLO_SMOKE=OFF` 关掉它(关了不影响行规范那些测试,那些不依赖 `/hello`)。

## 这章没做的

- **PTY / `/dev/ptmx` / `/dev/pts/N`**:master/slave 对硬依赖设备 inode 这一层,CinuxOS 这会儿没 DevFS,建了是空中楼阁。这一章用 console TTY 单例绕开,行规范 + 阻塞读 + EOF + 信号都齐了,PTY 留到 DevFS(后面)落地。
- **winsize 取真几何**:固定 80×25,因为 Console 是 `main.cpp` 局部变量、syscall 够不着。待 Console 全局化或 DevFS 给 fd 真设备身份。
- **shell 设前台组**:完整 job control 要 shell 在 fork 程序时主动 `TIOCSPGRP` 设前台组。现在的 shell 还没调它,所以 Ctrl+C 回退到阻塞读者(shell 自己)的组。musl/glibc 的 shell 会调;CinuxOS 自己的 shell 调不调待验。
- **这一章的改动里还顺带带了个 reap 的修复**:`-smp 2` 下子进程在别的核上退出、父进程 reap 的时序竞态(`reap_deferred` 加了 `on_cpu == -1` 同步),治一个 exit/reap 的栈 UAF #DF。它跟 TTY 没关系,是跟 TTY 同一批改动里一起落进来的,教学归到 SMP 那条线,这里不展开。

## 小结

- 之前的 stdin 是忙等轮询返 0(被 musl 当 EOF)、stdout 直接 kprintf、ioctl 全 `-ENOTTY`,交互式程序读不到真实输入。这一章补 TTY 行规范把「键盘 → 行规范 → 进程」接通。
- 行规范核心写成纯逻辑(回显走注入 callback、信号走枚举),换来 host 链真码单测;ICANON 攒行/退格/回车提交、ISIG 把 `^C`/`^\`/`^Z` 翻译成信号不进缓冲。
- 接键盘时两个坑:回显 sink 必须 lock-free(IRQ 上下文不能拿锁打印);设备层字节(`^H`)跟 UAPI 默认值(DEL)对不上,初始化时显式对齐 VERASE。
- 阻塞读用 `prepare_to_wait` + `schedule_blocked` 替忙等,关中断下「检查 + 登记 + 标 Blocked」原子防丢失唤醒;EOF 当状态(`eof_pending_` + `take_eof` 一次性),跟「暂时没行」分开。
- ioctl 接成真命令(TCGETS/TCSETS/TIOCGWINSZ),经 061 的 accessor 走、坏指针 `-EFAULT`;Ctrl+C 经 `killpg` 投前台组,终端能打断程序了。
