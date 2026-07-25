---
title: 01 · TTY 行规范:让 shell 真正能交互
---

# TTY 行规范:让 shell 真正能交互

> 上一章(059)让内核跑起了用真 musl 编译的静态程序,`hello` 能打出 `Hello from musl` 干净退出了。可一旦你想跟它**交互**——比如 `scanf` 读你敲的输入——就读不到。因为 stdin(`fd==0`)的实现是键盘 PS/2 环形缓冲上的**忙等轮询**:没键就空转一百万次,然后返回 0;而 musl 把「read 返回 0」当成 **EOF**,于是交互式程序一上来就以为输入结束了。stdout 那边也没好到哪去:`fd==1` 直接走 `kprintf`,没有行缓冲;`ioctl` 不管问什么都返回 `-ENOTTY`,musl 探窗口大小(TIOCGWINSZ)失败就退回全缓冲,`printf` 的输出不及时。键盘敲一个字符就交给程序一个字符,没有退格编辑、没有回车成行提交、没有 Ctrl+C——离一个真终端差远了。
>
> 这一章补一个真的 **TTY 子系统**,把「键盘 → 行规范 → 进程」这条链接通。行规范(line discipline)是终端的灵魂:它在原始按键和程序看到的输入之间加一层加工——攒一行、处理退格、回车才提交、把 Ctrl+C 翻译成信号。然后接上阻塞读(让 shell 等 stdin 时 CPU 不空转)、接上真 ioctl(答上 musl/glibc 的探针)、把 Ctrl+C 接成真的 SIGINT 投给前台进程组。做完这些,shell 就是个能用的交互终端了:本章的 punchline 就是这个——敲退格能编辑、回车提交整行、Ctrl+C 打断前台、Ctrl+D 结束输入。
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

地基倒是大多现成:信号、进程组、`killpg` 已经做完;进程结构里还预埋了一个 `Task::controlling_tty` 字段(没终端时是 -1);Console 已经能当 echo 的 sink。差的就差一个**真 TTY 子系统**,把键盘 → 行规范 → 进程这条链接起来。
