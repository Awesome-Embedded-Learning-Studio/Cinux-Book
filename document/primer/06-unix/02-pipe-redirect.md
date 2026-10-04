---
title: 02 · 管道与重定向
---

# 02 · 管道与重定向:一根竖线,交回来的是两个数字

咱们每天都在命令行里敲那根竖线。`ls | wc -l`,`ps -e | grep bash`,顺手就来,屏幕上的结果也从来不挑人。笔者这么顺手过很久,直到有一回写了个小循环,数出来的东西对不上:

```text
$ bash -c "n=0; echo a b c | while read -r _; do n=\$((n+1)); done; echo n=\$n"
n=0
```

`a b c` 是一次喂进去的,`while` 一遍就读完了。它数的是自己那个 `n`,跟咱们眼前这个 `n` 不是同一个东西:那根竖线把右边的循环搬到了另一个进程里,它那个 `n` 数到了 1,却留在了那边,咱们这边看到的还是 0。把命令交给 `strace`,屏幕上会掉出几行系统调用,那根竖线的底细就在这几行里。

## 管道在手里,就是两个数字

咱们写一个十五行的小程序,一个第三方依赖都不用:

```c
/* 管道的两头是两个 fd:pipe() 交回来的一对数字 */
#include <stdio.h>
#include <unistd.h>

int main(void) {
    int p[2];
    if (pipe(p) == -1) { perror("pipe"); return 1; }
    printf("pipe() 交给我的两个数字:p[0]=%d(读端) p[1]=%d(写端)\n", p[0], p[1]);
    printf("往 p[1] 写 6 个字节之后,从 p[0] 读出来:\n");
    if (write(p[1], "hello\n", 6) != 6) { perror("write"); return 1; }
    char buf[16];
    ssize_t n = read(p[0], buf, sizeof buf);
    printf("read(%d, buf, 16) = %zd,内容是 %.*s", p[0], n, (int)n, buf);
    return 0;
}
```

存成 `pipefd.c`,咱们编出来跑一遍:

```text
$ ./pipefd
pipe() 交给我的两个数字:p[0]=3(读端) p[1]=4(写端)
往 p[1] 写 6 个字节之后,从 p[0] 读出来:
read(3, buf, 16) = 6,内容是 hello
```

咱们在这一篇里贴出来的几支小程序,都用 `gcc -o 名字 名字.c` 编出来,不写额外开关。下面那些屏里的命令行,程序名用的是咱们自己存的名字,有的跟笔者当初一样,有的换过,脚本从笔者原来的路径挪到了当前目录,命令的内容、参数和开关没动。

这里没有“一个叫管道的对象”交给咱们,交回来的就是两个非负整数,一个读端,一个写端。往 `p[1]` 写六个字节,从 `p[0]` 读回来正好六个字节,`write` 和 `read` 的返回值都是 6,可数。

笔者手边的机器交回来的是 3 和 4,您那边可能是别的号。有三条得交代清楚:登进来的身份是 `uid=1000`,手里没有 root,根目录 `/` 是只读挂载,想往盘上写东西,只能落在当前目录。它跑在一套自己的 PID 名字空间里,`ps -e` 数下来只有几条。它也没有任何控制终端,`tty` 回给咱们一句 `not a tty`。下面每一个 fd 号、进程号和字节数,读的都是本机此刻的状态,换成您手上正常的 Linux,数字和权限都会不一样。咱们能稳住的只有关系和形状:一根管道交回两个 fd,一头只能写,一头只能读。

咱们凭什么说六个字节是可数的?因为管道送出去的是一串字节,它不认得消息。手册里这一句说得最干脆:

> The communication channel provided by a pipe is a **byte stream**: there is no concept of message boundaries.

## 两头顶上,父子各占一头

咱们把一根管道建出来,前面那段程序是自己写自己读。真要让它干点活,得有人往上头接。下面这段程序把管道建好,再 `fork` 一次,咱们让孩子占写端,父亲占读端:

```c
/* fork 之后父子各占一头:p[1] 写、p[0] 读 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    int p[2];
    if (pipe(p) == -1) { perror("pipe"); exit(1); }
    pid_t pid = fork();
    if (pid == -1) { perror("fork"); exit(1); }
    if (pid == 0) {                    /* 孩子:写端 */
        close(p[0]);                   /* 用不到的那一头要关掉 */
        const char *msg = "从孩子那头过来的一行\n";
        write(p[1], msg, strlen(msg));
        close(p[1]);
        _exit(0);
    }
    close(p[1]);                       /* 父亲:读端 */
    char buf[256];
    ssize_t n = read(p[0], buf, sizeof buf);
    printf("父亲读到 %zd 个字节:%s", n, buf);
    close(p[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    printf("孩子退出码 = %d\n", WEXITSTATUS(st));
    return 0;
}
```

上面这段源码是照原样搬的,头文件和四条注释都在这儿。咱们把它存成 `forkpipe.c`,编出来跑:

```text
$ ./forkpipe
父亲读到 31 个字节:从孩子那头过来的一行
孩子退出码 = 0
```

父亲读到 31 个字节,那是十个汉字加一个换行,按 UTF-8 一个汉字三个字节算出来的数。这个 `31` 与内容有关,跟机器无关,您那边也是 31。咱们真正要看的是那句注释:`close(p[0])` 与 `close(p[1])` 各自关掉自己用不到的那一头,父亲也把自己手里的写端关掉了。这两下关得对不对,决定了后头读不读得到结尾。

管道是字节流这件事,咱们还能看到一屏更刺眼的现场。同一根管道上,写的那头一次写 33 个字节,读的那头只肯读 8 个:

```c
/* 同步阻塞的管道:先写满 -> 读端只读一点 -> 内核在那里等你 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    int p[2];
    pipe(p);
    const char *msg = "一行一个字都还没被读走";
    printf("写之前:往 p[1] 写 %zu 字节\n", strlen(msg));
    write(p[1], msg, strlen(msg));
    char buf[8];
    ssize_t n = read(p[0], buf, sizeof buf);
    printf("只读 8 个字节 -> n=%zd:%.*s\n", n, (int)n, buf);
    return 0;
}
```

咱们把它存成 `blocking.c`,跑出来是这样:

```text
$ ./blocking
写之前:往 p[1] 写 33 字节
只读 8 个字节 -> n=8:一行�
```

写进去的那一串是十一个汉字,一共 33 个字节,读端只拿走了头 8 个。两个完整的汉字占掉 6 个字节,剩下 2 个字节是第三个汉字的开头,一个汉字要 3 个字节才拼得齐,终端拿着这半截去解码,末了那个位置就显示成 `�`。程序没有坏,是咱们只读了 8 个字节,把一个多字节的字从中间锯开了。管道不管字,也不管行,它只管把字节搬过去。

## 一条竖线在内核那边是六个系统调用

咱们前面讲内核是什么的那一篇里,已经拿 `strace` 看过一个程序的系统调用面,它的最小用法这里不再重讲,只是 `strace` 得您手边装一个。这回只把它对准一条带竖线的命令行。脚本叫 `pipe.sh`,里面就三行:

```bash
#!/bin/bash
export LC_ALL=C
echo a | wc -l
```

咱们把它存成 `pipe.sh`,加上可执行位(`chmod +x pipe.sh`)。屏里的命令行用的是咱们自己的脚本名,笔者当初的脚本放在自己的路径下,名字跟这里不一样,内容就是上面这三行。再请 `strace` 看:

```text
$ strace -f -e trace=pipe2,dup2,clone,execve ./pipe.sh
pipe2([3, 4], 0)                        = 0
[pid   197] dup2(4, 1)                  = 1
[pid   198] dup2(3, 0)                  = 0
[pid   197] +++ exited with 0 +++
```

`pipe2([3, 4], 0)` 那一行,就是一根管道到手,两个 fd 装进两个格子。往后那两行 `dup2` 才是全部机关,咱们一行一行看:左边那个子进程把管道的写端装到自己的 1 号上,右边那个子进程把管道的读端装到自己的 0 号上。装完,右边那个子进程换上 `wc` 去数行,原屏上这一步是 `execve("/usr/sbin/wc", ["wc", "-l"], …) = 0`——上面那个摘录只有四行,这一行在没搬的那几条里。左边那个子进程跑的是 shell 内建的 `echo`,内建就在子进程里直接执行,不再去磁盘上装一个程序,所以两个子进程那一趟里,`execve` 只有 `wc` 这一行。屏里的进程号、fd 号和地址都是本机此刻的,您那边是另一批,稳的是角色:左边拿写端,右边拿读端。于是一条 `echo a | wc -l` 落到内核那边,数下来是六个系统调用:一根管道 `pipe2`,两次 `clone` 把两个子进程起出来,两回 `dup2` 左右各摆一次 fd,最后一回 `execve` 只往一边装一个程序。

上面这一屏笔者只留了和管道直接相关的这几行,而且都是照原样搬的:脚本启动时的 `execve`、bash 读脚本留下的 `dup2(3, 255)`、`clone` 那两行、装 `wc` 的 `execve`、`wc` 数出来的那个 `1`、其余几行 `exited`,还有中间那行 `SIGCHLD`,都没搬。`clone` 那两行里,行中间还塞着 `strace: Process 197 attached` 和 `strace: Process 198 attached` 两处 `strace` 自己报的话,后头跟着一长串地址。至于 `fork` 怎么变成了 `clone`,这是眼下这套 libc 走的路,老一点的可能直接就是 `fork` 那个系统调用,咱们认的是“起子进程”这个动作。

这里还有一层和重定向接得上的关系,咱们来认一下:竖线两边那一对 fd,是在命令自己的重定向之前就摆好的。手册里写着:

> The standard output of command1 is connected via a pipe to the standard input of command2. **This connection is performed before any redirections specified by the command1.** …

所以咱们写 `echo a 2>/dev/null | wc -l` 的时候,`2>/dev/null` 改的是 `echo` 的 2 号,管道碰不到它。

## 管道有多大

管道两头一接,咱们很容易以为数据是一股脑过去的。它不是,中间有个有限的缓冲。下面这段程序去问内核这个缓冲有多大,再拿 `fcntl(p[1], F_SETFL, O_NONBLOCK)` 把写端设成非阻塞,一路灌到灌不动为止:

```c
#define _GNU_SOURCE
/* 管道有容量:非阻塞地灌,灌到 EAGAIN 为止,数一数灌进去多少 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    int p[2];
    pipe(p);
    printf("fcntl(p[1], F_GETPIPE_SZ) = %d\n", fcntl(p[1], F_GETPIPE_SZ));
    fcntl(p[1], F_SETFL, O_NONBLOCK);
    char buf[4096];
    for (size_t i = 0; i < sizeof buf; i++) buf[i] = '.';
    long total = 0;
    for (;;) {
        ssize_t n = write(p[1], buf, sizeof buf);
        if (n < 0) {
            printf("write 返回 -1,errno=%d (%s);此刻灌进去 %ld 字节\n", errno, errno == EAGAIN ? "EAGAIN" : "别的", total);
            break;
        }
        total += n;
    }
    struct pollfd q = {.fd = p[0], .events = POLLIN};
    printf("poll(读端) 返回 %d(>0 表示有数据可读)\n", poll(&q, 1, 0));
    ssize_t got = read(p[0], buf, sizeof buf);
    printf("读掉 %zd 字节之后再写:write = %zd\n", got, write(p[1], buf, sizeof buf));
    return 0;
}
```

存成 `pipecap.c`,咱们跑出来的四个数是:

```text
$ ./pipecap
fcntl(p[1], F_GETPIPE_SZ) = 65536
write 返回 -1,errno=11 (EAGAIN);此刻灌进去 65536 字节
poll(读端) 返回 1(>0 表示有数据可读)
读掉 4096 字节之后再写:write = 4096
```

笔者手边的机器上,管道默认装 65536 字节,灌满之后再写就返回 -1,`errno` 是 11,也就是 `EAGAIN`。从读端拿走 4096 字节,写端立刻又能写进 4096。这个容量不是恒定值,它可以被改大改小,咱们这里只读了默认值。所以咱们说“默认 64 KiB”,不说“管道就是 64 KiB”。`65536` 正好是 `16 × 4096`,而 `4096` 也有名字:

> **PIPE_BUF** — POSIX.1 says that **writes of less than PIPE_BUF bytes must be atomic** … (**On Linux, PIPE_BUF is 4096 bytes.**)

一次写不超过 4096 字节,内核保证它不会被别的写插进中间去。这一点咱们只引手册,没有逐条跑阻塞与非阻塞的四种组合。

还有一件事得跟您交代:上面这段程序没有第二个读者,它自己灌自己读,所以灌到 65536 就停下。真实的管道里,读端那头一直在往外拿,写端那头就能一直往里放,数据过手不停留。`EAGAIN` 只在把写端设成非阻塞的时候才出现,默认是阻塞的,满了就是卡在那儿等,屏幕上一动不动。

## 管道只报最后一段的脸色

管道把一个作业分成几段分别跑,可 shell 只交回一个退出码。`yes` 会让 `head -1` 提前走人,咱们敲下 `yes | head -1` 之后,`$?` 按最后一段算是 0,`PIPESTATUS` 数组里却是另一个故事:

```text
$ yes | head -1
y
yes: standard output: Broken pipe
$ echo ${PIPESTATUS[*]}
1 0
```

笔者用的脚本把同一条命令跑了两遍,第二次那一遍自己也当场打了 `y` 和 `yes: standard output: Broken pipe` 两行,这里都省掉了,只留 `1 0` 那一行读数。左边一格是 1,右边一格是 0。`$?` 只报最后一段,`head` 成功了,所以整条管道看着一路绿灯。要让它报出左边那一段,得把 `pipefail` 打开:

```text
$ bash -c "false | true; echo \$?"
0
$ bash -c "set -o pipefail; false | true; echo \$?"
1
```

手册把这两句写在一起,咱们直接引原话:

> The return status of a pipeline is the exit status of the **last command**, unless the **pipefail** option is enabled.

左边一格那个 1 是怎么来的,咱们放到讲写断管道的地方去看。这里还有第三格,也是这一篇里最容易把人绊倒的地方:管道会给每一段都起一个子 shell。开头那个 `n=0` 就是这么来的,同样的循环换成进程替换,数就对了:

```text
$ bash -c "n=0; echo a b c | while read -r _; do n=\$((n+1)); done; echo n=\$n"
n=0
$ bash -c "n=0; while read -r _; do n=\$((n+1)); done < <(echo a b c); echo n=\$n"
n=1
```

右边那个 `while` 跑在一个新起的子 shell 里,这一点咱们已经知道:它在自己那边把 `n` 加到了 1,外面看不见这一步。然后它带着这个 1 退出,外面那个 `n` 从头到尾没被碰过。`< <(…)` 那一版把 `while` 留在当前 shell 里跑,只有数据是被管道送进来的,所以 `n` 留下来了。

## 写进一根没有读者的管道

管道那头的人走了,咱们还往上头写,会发生什么?同一支程序、同一个动作,四种处置给出四种结果。程序里那个模式参数决定它把 `SIGPIPE` 摆成哪一档(源码第一行那句注释数的是程序主动摆的三档,第四档是继承,程序不动它):

```c
/* 同一件事,三种信号处理方式:默认 / 挡住 / 自己的处理函数 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void on_pipe(int sig) {
    (void)sig;
    const char *m = "[自己的处理函数被叫到了]\n";
    write(2, m, strlen(m));
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "default";
    if (strcmp(mode, "dfl") == 0) { signal(SIGPIPE, SIG_DFL); printf("SIGPIPE -> SIG_DFL(默认)\n"); }
    else if (strcmp(mode, "ign") == 0) { signal(SIGPIPE, SIG_IGN); printf("SIGPIPE -> SIG_IGN(挡住)\n"); }
    else if (strcmp(mode, "own") == 0) { signal(SIGPIPE, on_pipe); printf("SIGPIPE -> 自己的处理函数\n"); }
    else if (strcmp(mode, "inherit") == 0) { printf("SIGPIPE 不动(继承启动者的设置)\n"); }
    else { printf("未知模式\n"); return 2; }
    fflush(stdout);
    int p[2];
    pipe(p);
    close(p[0]);
    char buf[65536];
    memset(buf, 'x', sizeof buf);
    ssize_t n = write(p[1], buf, sizeof buf);
    if (n < 0) printf("write 返回 -1,errno=%d (%s)\n", errno, strerror(errno));
    else printf("write 返回 %zd\n", n);
    return 0;
}
```

它把自己那头的读端关掉,等于没有人再来读,然后一次写 65536 字节。咱们把它存成 `sigpipe.c`,把这四档处置跑下来。第一档,程序把处置摆回默认:

```text
$ ./sigpipe dfl; echo "退出码=$?"
SIGPIPE -> SIG_DFL(默认)
退出码=141
```

第二档,咱们把信号挡住:

```text
$ ./sigpipe ign; echo "退出码=$?"
SIGPIPE -> SIG_IGN(挡住)
write 返回 -1,errno=32 (Broken pipe)
退出码=0
```

第三档,咱们挂上自己的处理函数:

```text
$ ./sigpipe own; echo "退出码=$?"
SIGPIPE -> 自己的处理函数
[自己的处理函数被叫到了]
write 返回 -1,errno=32 (Broken pipe)
退出码=0
```

第四档,咱们什么都不动,让它继承启动者的设置:

```text
$ ./sigpipe inherit; echo "退出码=$?"
SIGPIPE 不动(继承启动者的设置)
write 返回 -1,errno=32 (Broken pipe)
退出码=0
```

头一格是默认处置,咱们看到的是:内核给进程投一个 `SIGPIPE`,进程当场被收掉,shell 看到的退出码是 141。这个 141 是算出来的,`128 + 13`,`13` 正是 `SIGPIPE` 的编号。第二格把信号挡住,进程不死,`write` 自己回来报错,`errno` 是 32,也就是 `EPIPE`,退出码 0。第三格挂上自己的处理函数,函数被叫到一次,`write` 照样报 `EPIPE`。第四格什么都不动,结果跟第二格一样。

第四格的结果,笔者手边的机器上有一个必须交代的背景。**它出厂就把 `SIGPIPE` 挡住了**。把 `/proc/self/status` 里那个 `SigIgn` 掩码读出来,是 `0000000001001000`。从低位往上数,第 13 位和第 25 位是 1,翻成信号名就是 `SIGPIPE` 和 `SIGXFSZ`。下面这一行不是 `status` 的原样,箭头和信号名是笔者拿一支小脚本把那个掩码逐位译出来的:

```text
  SigIgn = 0000000001001000  ->  13(SIGPIPE) 25(SIGXFSZ)
```

`SigIgn` 这一行说明,13 号信号在笔者手边的机器上一直是忽略,所以照着上面的写法走,第四格拿到的是 `退出码=0`,而不是头一格的 141。换到您手上正常的 Linux,`SIGPIPE` 是默认处置,同一个程序会被信号收掉,退出码是 141。想让结果跟头一格一样,程序里那句 `signal(SIGPIPE, SIG_DFL)` 就不能省,它写在程序开头那一档分支里,`dfl` 这个模式一进去就摆上。

内核那一侧的样子,咱们请 `strace` 也留了一屏:

```text
$ strace -e trace=write ./sigpipe dfl 2>&1 | tail -4
) = 27
write(4, "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"..., 65536) = -1 EPIPE (Broken pipe)
--- SIGPIPE {si_signo=SIGPIPE, si_code=SI_USER, si_pid=186, si_uid=1000} ---
+++ killed by SIGPIPE +++
```

头一行是被 `tail` 切掉前半截的一条 `write`。`write` 自己失败,报回 `EPIPE`,紧跟着才是那个信号,末了一行 `killed by SIGPIPE`。数字里的进程号是本机此刻的,您那边不是同一个。

同一个场景,咱们还能看到另一条岔路:GNU 的工具不装默认处置,而是自己把 `write` 的报错接过去,报一句错、退一个 1 就完事。上面那个 `yes` 就是这么干的(版本号 9.12 出自笔者手边的机器):

```text
$ yes --version | head -1
yes (GNU coreutils) 9.12
$ yes | head -1; echo "PIPESTATUS=${PIPESTATUS[0]}"
PIPESTATUS=1
```

这一句 `yes | head -1` 在标准错误上打的那两行,咱们在上面已经见过,一模一样,就不再印第二遍了,只留最后那一行读数。

所以 `yes | head -1` 里那个 1,不是 141,是工具自己报错退出来的。库和工具替咱们接了这一手,和信号把进程收掉,是两码事。

## 什么时候算读完

读端怎么知道那头写完了?咱们看 `read` 的返回值,0 就是答案。返回 0 的条件只有一条:再没有谁可能往里写了。下面这段程序让孩子写完三个字节以后不关写端,睡两秒再关:

```c
/* "读端什么时候算读完":所有写端都关掉才给 EOF */
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    int p[2];
    pipe(p);
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        write(p[1], "abc", 3);
        /* 这里故意不关 p[1],看看父亲会不会先收到 EOF */
        printf("孩子写完了,但还没关写端\n");
        sleep(2);
        printf("孩子关写端\n");
        close(p[1]);
        _exit(0);
    }
    close(p[1]);
    char buf[16];
    ssize_t n = read(p[0], buf, sizeof buf);
    printf("父亲第一次 read = %zd\n", n);
    n = read(p[0], buf, sizeof buf);
    printf("父亲第二次 read = %zd (0 才是 EOF)\n", n);
    wait(NULL);
    return 0;
}
```

咱们把这一段存成 `eofwait.c`,在笔者手边的机器上跑出来只有父亲那两行:

```text
$ ./eofwait
父亲第一次 read = 3
父亲第二次 read = 0 (0 才是 EOF)
```

孩子那两句 `printf`,在笔者手边的机器上一行都没上屏。这里没有控制终端,stdout 接的是一份文件,那两句话攒在 stdio 自己的缓冲里,`_exit` 一走就跟着散了。您那边把 stdout 接在终端上跑同一支程序,这两行当场就冒出来,`fflush(stdout)` 加不加都行。要让它们在这里也上屏,才得在孩子那边补一句 `fflush(stdout)`。咱们能读到的就是父亲这两行:第一次读到 3 个字节,第二次读到 0。中间那两秒孩子还睡着、写端还开着,父亲就停在第二个 `read` 上等,孩子一关写端,0 立刻回来。要是孩子拿住写端不放,睡足五秒:

```c
/* 忘了关写端:父亲会一直等下去 */
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    int p[2];
    pipe(p);
    pid_t pid = fork();
    if (pid == 0) {
        write(p[1], "abc", 3);
        printf("孩子写完,拿住写端不放,睡 5 秒\n");
        fflush(stdout);
        sleep(5);
        _exit(0);            /* 忘了 close(p[1]) —— _exit 会关掉它 */
    }
    close(p[1]);
    char buf[16];
    ssize_t n = read(p[0], buf, sizeof buf);
    printf("父亲的 read 现在才回来 = %zd\n", n);
    wait(NULL);
    return 0;
}
```

咱们把这一段存成 `noclose.c`,孩子写完三个字节不关写端,睡满五秒才退,父亲那头就一直等:

```text
$ time ./noclose
孩子写完,拿住写端不放,睡 5 秒
父亲的 read 现在才回来 = 3

real	0m5.002s
user	0m0.001s
sys	0m0.000s
```

父亲在那一次 `read` 上等了五秒,等孩子一退、`_exit` 替他关掉写端,数据才交出来。这个 5.002 秒是笔者手边机器掐出来的墙上时间,您那边会差上几个毫秒。`user` 和 `sys` 两行跟这一拍无关,咱们看的是 `real`。这一次它把已经到手的三个字节交了出来,而不是 0,这不是每一家内核都一样的边界,咱们按这一次看到的样子写。

咱们再看更极端的一格,父亲自己手里也留着一份写端。读端要等到“所有写端”都关掉,自己手里留着的也算数,于是孩子的 `read` 永远等不到那个 0:

```c
/* 最常见的那个版本:p[1] 在父亲手里一直开着,孩子的读永远等不到 EOF */
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    int p[2];
    pipe(p);
    pid_t pid = fork();
    if (pid == 0) {
        close(p[1]);              /* 孩子只读 */
        char buf[16];
        ssize_t n = read(p[0], buf, sizeof buf);
        printf("孩子 read = %zd(想要 0)\n", n);
        fflush(stdout);
        _exit(0);
    }
    printf("父亲手里还开着写端 p[1]=%d ……\n", p[1]);
    fflush(stdout);
    int st = 0;
    waitpid(pid, &st, 0);
    printf("孩子是被 %d 号信号收掉的\n", WTERMSIG(st));
    return 0;
}
```

咱们把这一段存成 `leftover.c`。这一回父亲不会自己退出,孩子也永远等不到 0,要让实验停下来,得请 `timeout` 来收场:

```text
$ timeout 3 ./leftover; echo "退出码=$?  (124 = timeout 到点收掉)"
父亲手里还开着写端 p[1]=4 ……
退出码=124  (124 = timeout 到点收掉)
```

那行 `p[1]=4` 里的号是本机此刻的,您那边是另一个数。这个 124 是 `timeout` 自己的约定,不是程序算出来的退出码。**要读到这一屏,得靠 `timeout` 才停得下来**,您照着敲的时候别让它真的挂在那儿,`3` 那个秒数按您自己的耐心改。

到这里,咱们就能把这件事说成一句了:`EOF` 不是写端发过来的一个消息,是内核发现再没有谁可能往里写了。关掉自己用不到的那一头,是让这个判定成立的前提。

同一个机制在文件系统里另有一个名字,咱们用 `mkfifo` 把它建出来:

```text
$ mkfifo mypipe; ls -l mypipe
prw-r--r-- 1 ‹您的用户名› ‹您的用户组› 0 ‹您那一刻的时间› mypipe
$ (echo "从另一个进程过来" > mypipe &) ; cat mypipe
从另一个进程过来
```

`ls -l` 交回来的那一行,第一个字母是 `p`,后面跟着 `rw-r--r--`,认这第一个字母就知道它不是普通文件。这一行剩下的属主、用户组和时间戳,笔者手边那台机器上是它自己的名字和当时的时间,您那边是您的,咱们用尖括号把这三格顶掉了,只认最前面那个字母。数据不落在盘上,内核在两个进程之间直接把它递过去。上面那句命令会在您当前目录里真的建出一个叫 `mypipe` 的文件,试完把它删掉就行。

## 自己跑出来跟上面不一样

咱们照着上面这些片段动手,最容易遇上的几处不一样,按碰到的机会从多到少排一排。

- **循环里那个变量数出来是 0。**`while` 跑在管道右边的子 shell 里,它改的是自己那个变量,出不了管道。想让改动留下,换成 `< <(…)` 的写法,把 `while` 留在当前 shell 里。
- **`read` 迟迟不返回 0。**管道那头还有人开着写端,您自己手里留着的也算。把用不到的那一头关掉,再回去数一遍:同一个 fd 在父子两边各有一份,关掉父亲手里的,不影响孩子手里的。
- **同一条命令退出码是 0,不是 141。**笔者手边的机器出厂就把 `SIGPIPE` 挡住了,拿到 0 和 `EPIPE` 是正常的。要看到 141,程序里得写死 `signal(SIGPIPE, SIG_DFL)`,或者换一台 `SIGPIPE` 没被挡的机器。
- **灌到一半卡住不动。**上面那段容量程序把写端设成了非阻塞,所以满了会报 `EAGAIN` 回来。默认是阻塞的,写端满了就停在那儿等读者,没有读者就一直等。想复现容量那一屏,`fcntl(p[1], F_SETFL, O_NONBLOCK)` 那一行不能漏。
- **数字全对不上。**fd 号是 3 和 4 还是别的,容量是 65536 还是别的,进程号是几,读的都是本机此刻的状态。您那边换一批数字不奇怪,形状不变。

## 这一篇没有走到的地方

管道的容量可以改,`fcntl` 那个 `F_SETPIPE_SZ` 能把它调大调小,咱们只读了默认值,一次都没有改过,所以咱们说“默认 64 KiB”,不说“管道就是 64 KiB”。

`PIPE_BUF` 那一句手册还配了一张四种组合的表,分阻塞与非阻塞,再按一次写进去的量小于等于还是大于 `PIPE_BUF` 各分一种,咱们没有逐条跑,只引了原话。

管道缓冲在内核里怎么排队、满了之后谁被叫醒、读者从哪一刻开始跑,这些都是调度和内核那边的题目,咱们这一篇一个字都没有碰。

`strace -f` 出来的全脸咱们也没有照搬。子进程和父进程的行是交错在一起的,读起来一团乱,这一屏只挑了和管道直接相关的那几行。

管道和后面的 socket 共用同一个 fd 面,咱们写进去、读出来、关掉,形状是一样的。那边要等到讲两个进程隔着 socket 说话的时候再展开。

## 双视角锚点:同一根竖线,两个人读到的不是一回事

**那一根竖线。**应用程序员敲下 `ls | wc -l`,他看到的是两个命令接上了,前一个把东西吐出来,后一个接住,这一趟就完了,剩下的都是屏幕上的事。轮到咱们写内核的作者这一侧,同一根竖线分成两样东西:两根 fd 加一个内核缓冲,再加一个判断,还有没有谁可能往里写。`pipe2` 交回两个数字,`dup2` 把写端装进 1 号、把读端装进 0 号,数据从那块缓冲里过一手。应用侧看到的是接力,系统侧看到的是两根管口对着一块有限的缓冲,外加一条“谁还可能写”的判定。`write` 返回 6、`read` 返回 6,两次返回值就是那一趟的全部。

**那个退出码。**应用程序员在脚本里写下 `a | b`,他看到的是一个作业,退出码不是 0 就是不对劲,应该有人接住。轮到咱们内核作者这一侧,管道是一堆各自退场的进程,shell 手里只有一个位置放退出码,它放的是最后一段的那个,前面几段的脸它不看,除非您把 `pipefail` 打开,或者回头去翻 `PIPESTATUS`。一边看到的是成功或失败,一边看到的是只报最后一段的脸色。

**那一头没人读。**应用程序员那边,写不进去就是写不进去,拿到一个错误,处理掉接着跑。轮到咱们内核作者,写端对着一个没有读者的管道,内核投的是一个信号,默认处置会把整个进程收掉,连返回错误的机会都不给,除非程序自己把这一档改掉。同一件“那头没人了”,一边看到的是一个返回值,一边看到的是一次投递。

下一篇咱们把这一根竖线底下跑着的两个进程接过来:它们从哪里来,退场的时候又是谁替它们把名字收掉。
