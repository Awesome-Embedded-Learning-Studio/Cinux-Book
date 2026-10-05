---
title: 04 · 信号
---

# 04 · 信号:一个字节走两条路

信号跟咱们前面几篇摸的东西不是一路货。管道、文件描述符、`/proc` 里那些号,都是数据或者数据的去处。信号不是数据,它是**一件事发生了的通知**。TLPI(《Linux/UNIX 系统编程手册》)给的定义是“A signal is a notification to a process that an event has occurred”,后面还补了半句,说信号有时候被描述成软件中断。把通知跟数据分开,这一篇就顺了。

笔者把自己手头一台机器的三条底细交出来,因为这一篇里好几处现象会被它改掉。咱们这边跑在一个受限环境里:当前 uid 是 1000,没有 root,`/` 挂的是只读,您去 `touch /x` 会当场收到一句 Read-only file system,这不是笔误。`ps -e` 拉出来只有个位数的几条,进程号都是个位到三位数,您那边多半是一屏拉不到头,因为咱们这个环境有自己的一份 PID 名字空间。最后一条最要命,本机默认**没有控制终端**,`tty` 直接答 not a tty,想要真终端就得现开一个 pty(伪终端),咱们下面有一节就得这么干。本机的 `SIGPIPE` 出厂就是被挡住的 —— 这件事笔者记在这儿,讲到它的时候再展开。

## 术语三拍:产生、挂着、送到

咱们看 `signal(7)` 讲信号掩码的那一段,它是这么说的:“A signal may be blocked, which means that it will not be delivered until it is later unblocked. Between the time when it is generated and when it is delivered a signal is said to be pending.” 这段话里,一个信号有三个位置:`generated`(产生)、`pending`(挂着)、`delivered`(送到)。后面几节要用的词就是它们三个。

咱们分开说:

1. **产生(generated)**:内核、别的进程、或者终端驱动,往这个进程头上放了一个信号。
2. **挂着(pending)**:这会儿进程正挡着它,信号暂时记下,不送。
3. **送到(delivered)**:挡板一撤,信号立刻兑现,该跑的处置跑起来。

挂着和送到之间能塞进一段真时间,这就是咱们下面要数出来的东西。`signal(7)` 还交代了一句,处置是**进程级**的属性,咱们每个信号各有一份当前处置,默认动作、忽略、以及自己装的函数,都记在进程自己身上。它还捎带说了 `execve` 的限制:有处理函数的那些处置会被重置成默认,而被忽略的那些**原样留着**。这个限制也解释了,为什么启动者挡住的信号会一路跟到它的子程序里去,咱们待会儿就用得上。

## 咱们让一个进程自己给自己发一个信号

要写的程序一个文件就够,存成 `sig-trip.c`,四十二行。笔者给它安排的动作是:给 `SIGUSR1` 装上处理函数,再把 `SIGUSR1` 挡在门外,然后 `kill` 自己一下:

```c
/* 信号的三拍:generated -> pending -> delivered */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t got = 0;
static void handler(int sig) { got = sig; }

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "default";
    pid_t pid = getpid();
    printf("我的 pid=%d,等信号(模式 %s)\n", pid, mode);
    fflush(stdout);
    if (strcmp(mode, "trap") == 0) {
        struct sigaction sa = {0};
        sa.sa_handler = handler;
        sigaction(SIGUSR1, &sa, NULL);
        printf("SIGUSR1 装了处理函数;现在把 SIGUSR1 挡住(sigprocmask),再 kill 自己\n");
        fflush(stdout);
        sigset_t set, old;
        sigemptyset(&set);
        sigaddset(&set, SIGUSR1);
        sigprocmask(SIG_BLOCK, &set, &old);
        kill(pid, SIGUSR1);
        sigset_t pend;
        sigpending(&pend);
        printf("挡住之后 sigpending(SIGUSR1) = %d (非 0 = 挂着没送)\n", sigismember(&pend, SIGUSR1));
        fflush(stdout);
        sleep(2);
        printf("解挡之前还挂着吗 = %d\n", sigismember(&pend, SIGUSR1));
        fflush(stdout);
        sigprocmask(SIG_SETMASK, &old, NULL);
        printf("解挡之后 got=%d(处理函数跑过了)\n", (int)got);
        return 0;
    }
    if (strcmp(mode, "ignore") == 0) { signal(SIGUSR1, SIG_IGN); }
    for (int i = 0; i < 100; i++) pause();
    return 0;
}
```

咱们在自己建的工作目录里编它、跑它:`gcc -O0 -o sig-trip sig-trip.c`,再 `./sig-trip trap`。

这是本机此刻读到的,您那边会换一批号:

```text
我的 pid=324,等信号(模式 trap)
SIGUSR1 装了处理函数;现在把 SIGUSR1 挡住(sigprocmask),再 kill 自己
挡住之后 sigpending(SIGUSR1) = 1 (非 0 = 挂着没送)
解挡之前还挂着吗 = 1
解挡之后 got=10(处理函数跑过了)
```

那两行 `= 1` 就是“挂着”这一步。`kill` 已经把信号放出去了,`sigpending` 也认下说它挂在那里,可处理函数一次都没跑。中间那两秒 `sleep` 不是摆设,它是这个“挂着”唯一的时间证据。等 `sigprocmask(SIG_SETMASK, &old, NULL)` 把挡板撤掉,处置立刻兑现,`got` 变成 `10`。`10` 是本机上 `SIGUSR1` 的号,常见的 x86 机器都是这个数。个别架构上它的号不一样,想让程序在哪儿都成立,您就用信号名,别把数字写死。

## 一趟信号,三种处置

同一个信号落到进程手上,行为由**进程自己**选。上面那个程序里咱们还留了两个模式:一个把 `SIGUSR1` 设成忽略,另一个什么都不做,只让 `pause()` 把它按在原地等(`pause()` 就是就地睡下,等一个信号来叫醒它):

| 进程选的那一档 | 会发生什么 |
|---|---|
| 默认处置(`SIG_DFL`) | 按信号自己的默认动作办,`SIGUSR1` 的默认动作是终止进程 |
| 忽略(`SIG_IGN`) | 信号被丢掉,进程原地不动,`pause()` 接着等 |
| 自己的处理函数 | 函数被叫起来,进程接着跑,`got` 记下是哪个信号 |

停在一个空的 `pause()` 里的进程,跟“什么都没发生的进程”,在花名册上长得差不多,`STAT` 那一栏通常就是个 `S`。咱们别被这一栏骗过去。信号“产生”不等于“送到”,前者随时可能发生,后者要等进程自己允许。

咱们前面在管道那一篇里已经拿 `SIGPIPE` 演过同一张表,这里不重复。要提醒您的只有本机的环境差:本机的 `SIGPIPE` **出厂就是被挡住的**,挡它的掩码就是管道那一篇里读到的那一行 `SigIgn`。普通机器上它是默认处置,同一个程序会被信号收掉,shell 报 `141`。差别就在那一行掩码,换台机器就换个数,咱们认的是“处置记在进程身上”这个形状。

处置能看见,也能改。改它用的是 `sigaction`,老一点的代码会用 `signal`。想看它,读 `/proc/<pid>/status` 里三个掩码字段就够了,`SigBlk` 是挡着的、`SigIgn` 是忽略的、`SigCgt` 是抓着的。三个字段的值都是一串十六进制位,哪一位对应哪个信号,您按位读过去就行。

## `kill` 只管送,送给谁看 pid 参数

`kill` 命令行里那个 `pid` 参数写着送给谁,咱们前面给的都是一个具体的号。`kill(1)` 把这个参数分成四档:

- 正数 `n`,送给 PID 为 `n` 的那个进程。
- `0`,送给**当前进程组里的所有进程**。
- `-1`,送给所有 PID 大于 1 的进程,`kill(1)` 的原话是 “All processes with a PID larger than 1 are signaled.”,`kill(2)` 说得更细:有权限送的都送,1 号 init 和调用者自己除外。
- 负数 `-n`,送给进程组 `n` 里的所有进程。

`0` 这一档值得您单独看,因为它是“整组一起”。下面这个程序也写全,存成 `sig-group.c`。组长用 `setpgid(0, 0)` 把自己单独立成一组,再 `fork` 出三个成员。三个成员各自停在 `pause()` 里,组长睡过一秒,往 `0` 号发一个 `SIGTERM`。

```c
/* 发给进程组:谁收到 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
    setpgid(0, 0);                       /* 自成一组的组长 */
    printf("组长 pid=%d pgid=%d\n", getpid(), getpgrp());
    fflush(stdout);
    for (int i = 0; i < 3; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            printf("成员 pid=%d pgid=%d\n", getpid(), getpgrp());
            fflush(stdout);
            for (;;) pause();
        }
    }
    sleep(1);
    printf("往整组(0 号)发 SIGTERM\n");
    fflush(stdout);
    kill(0, SIGTERM);
    sleep(1);
    printf("我居然还活着\n");
    return 0;
}
```

编出来跑一遍:`gcc -O0 -o sig-group sig-group.c`,再 `./sig-group; echo "退出码=$?"` —— 后面那个 `echo` 是为了把退出码收下来。本机此刻读到的是下面这几行,您那边是别的号:

```text
组长 pid=325 pgid=325
成员 pid=326 pgid=325
成员 pid=327 pgid=325
成员 pid=328 pgid=325
往整组(0 号)发 SIGTERM
Terminated                 ./sig-group
退出码=143
```

四个进程报出来的 `pgid` 是同一个号,这一点比号本身重要,它是“同组”的定义。屏上那行 `Terminated` 是 shell 报的,不是程序打的:它看见前台作业被信号收掉,就照您敲的那个名字念一遍。笔者当时跑那一次用的是另一个名字,别的字都一样,这一行咱们按 `./sig-group` 写。然后咱们就没有读到“我居然还活着”那句,`kill` 后面虽然还跟着一句 `printf`,组长已经轮不到它。shell 看到的 `143` 是**算出来的**,`128` 加上 `SIGTERM` 的号 `15`。这个换算是 shell 那一侧的约定,跟信号自己的号有关,跟机器无关。`SIGTERM` 的默认动作是终止,所以不用谁接管,进程就退场了。那句 `setpgid` 是这一屏为了可复现自己加的:您在交互 shell 里敲命令的时候,本来就已经在一个进程组里,不用自己造。

您还可以更省事,不用写程序,直接在 shell 里敲:`kill -0`。这次 `0` 落在**信号号**那一栏,和上面那个 pid 参数管的是两回事:号给 `0`,就是一个信号都不送,只按送信号那一套查一遍在不在、有没有权限。

```text
$ kill -0 99999 2>/dev/null; echo "kill -0 不存在 = $?"
kill -0 不存在 = 1
$ kill -0 $$; echo "kill -0 自己 = $?"
kill -0 自己 = 0
```

咱们试两次,一个不存在的号回 `1`,自己的号回 `0` —— 信号一个都没出去,答案已经拿到了。同一个 `kill`,既能把一件事说给一个进程听,也能拿来做探针。死不死,要看那个进程给这个信号安排了什么处置。

## 同一个字节,两条路

前面三节里,信号要么是内核发的,要么是咱们自己发的。可日常最常按的那个 Ctrl-C,既不是内核主动发的,也不是咱们的代码发的。键盘只给出一个字节,`0x03`。这个字节往后走哪条路,取决于它从哪儿进来。

在真终端上按一下,走的是第一条路。要重现这一屏,得有一支跑得住、又每秒出一声的靶子,笔者写成了 `loud.sh`:

```bash
#!/bin/bash
export LC_ALL=C
echo "开始:pid=$$ 组=$(ps -o pgid= -p $$ | tr -d ' ') 前台组=$(ps -o tpgid= -p $$ | tr -d ' ')"
for i in 1 2 3 4 5 6 7 8; do
  echo "第 $i 秒:还在跑 $(ps -o stat= -p $$ | tr -d ' ')"
  sleep 1
done
echo "整整八秒都没人打断我"
```

然后给它现开一个 pty,隔 0.4 秒从主端喂进去一个 `\003`,用的就是 `script -qec` 那一手。您在自己的终端上不必这么绕,按一下 Ctrl-C 就行:

```text
$ (sleep 0.4; printf "\003") | script -qec "bash loud.sh" /dev/null
开始:pid=332 组=332 前台组=332
第 1 秒:还在跑 Ss+
^C
```

屏上三个号是本机此刻的,您那边是别的号。同一张靶子屏在讲终端那一篇里还会再引一次,那儿看的是终端驱动那一侧。`Ss+` 读的是状态:这个进程在等,而且它是这个会话的领头者,坐在前台。`开始:` 和 `第 1 秒:` 这两行是脚本自己打的。那行 `^C` **不是**脚本打的,它是终端驱动把控制字符回显成这个样子给咱们看的,回显这件事本身就是终端在办。咱们真正要认的,是**那一秒之后的一片空白**,本来能看到脚本一秒一行往下数,可“第 2 秒”再也没出现,进程在第 1 秒之后就被收掉了。把脚本收掉的那个信号是 `SIGINT`,而它是由终端驱动从这个字节翻译出来的,不是脚本从输入里读到了什么。`pty(7)` 里那句写得很直:“writing the interrupt character (usually control-C) to the master device would cause an interrupt signal (SIGINT) to be generated for the foreground process group that is connected to the slave.”

另一条路是把它塞进管道,咱们看同一个字节在管道里出来成了什么:

```text
$ printf "\003" | od -c
0000000 003
0000001
```

`od -c` 把一个字节写成三位的八进制 `003`,您看下面那行那个 `0000001`,它是这一行的字节计数,是一,不是内容。管道里没有终端驱动,没有行上的特殊字符开关,也没人负责翻译,所以这个字节走到哪都只是数据,`SIGINT` 一个都没发出去。

同一个字节,两条路的差别就落在一句话上:终端驱动把某个字节**翻译**成一个信号,投给**前台进程组**。`\003` 走 pty 是通知,走管道是数据。终端那一篇接的就是这里 —— 一串开关管着“哪个字节算特殊、什么时候才交出去”,咱们下一篇就看它们。

## 排障与欠下的一笔

下面两处,笔者建议您多留个心眼,不然照着做会对不上。

信号“产生”不等于“送到”,咱们已经数过一遍了。程序里只要还有没送出去的那个信号,咱们从 `kill` 返回的时候,信号完全可能还挂着。想确认,`sigpending` 是现成的口子。**别拿“进程还在跑”当成“信号已经处理了”**,那两件事在花名册上长得太像。

想把 `SIGUSR1` 换成别的号,您最好看一眼它的默认动作。`SIGUSR1` 的默认是终止,所以万一处理函数装失败,您的程序会当场消失,而您还在以为它在等。咱们写的那个小程序里,`sigaction` 的返回值被忽略了,这是故意的,真实代码里最好看一眼。

还有一笔欠下的要如实记下。APUE(《UNIX 环境高级编程》)在讲 `sleep1` 的地方演过一个“有缺陷的版本”,用 `alarm` 配 `pause` 睡一段,然后一条条讲老实现的毛病。这是个好手法,笔者也认。可**咱们这一轮没有真跑书里那两版**,手里只有书上的两页(APUE 书页 339–340)。所以这一篇里咱们不造那个现场,也不替它说实验结果,就记在这里,等有了真件再补。

最后回指一句就够了。前面讲并发的那一篇里,两条线程抢一个变量那个最小现场,咱们到这里不再重做。那里数的是两条线程抢同一个变量的次序,这里数的是一个信号挤在挡板后面。都是“中间有个空档”,只是主角换了。
