---
title: 01 · 内核是什么
---

# 01 · 内核是什么:一条命令,机器替咱们说了 34 句话

咱们拿一小段 C 写一个只会打印一行字的程序,存成 `hello.c`:

```c
#include <stdio.h>

int main(void) {
    printf("hello\n");
    return 0;
}
```

咱们编它、跑它、把它跟内核说的话录下来,三行命令就够(都在您存 `hello.c` 的那个目录里跑)。`strace` 的 `-o` 那一手是把话写进文件,省得程序的输出和 trace 挤在同一个屏上:

```bash
gcc -O0 -o hello hello.c
strace -o trace-hello.txt ./hello
grep -c . trace-hello.txt
```

第三行数出来 36。程序自己只写了一行 `printf`,录下来的 `trace-hello.txt` 里却有 36 行。咱们看它的尾巴:

```text
...
write(1, "hello\n", 6)                  = 6
exit_group(0)                           = ?
+++ exited with 0 +++
```

这 36 行是这么分的:系统调用占 35 条,末尾那一行 `+++ exited with 0 +++` 是 strace 自己加的收尾。这一屏是笔者在自己机器上读到的,`36` 这个数跟着 libc、动态链接器和标准输出的去向走,您那边不会一样:同一台机器上,`./hello` 的输出接在管道上是 36 行,重定向到 `/dev/null` 会多出一条 `ioctl(1, TCGETS2)`,变成 37。数字是本机此刻的数字,稳的是形状:一行一次调用,写成 `名字(参数) = 返回值`。那行 `write(1, "hello\n", 6) = 6` 是整份录屏里唯一一件咱们本来就想干的事,往 1 号(标准输出)写六个字节,前面那三十来行全是“为了让它能干这件事”。

次数不必靠数行,`strace -c` 直接给咱们一张表:

```bash
strace -c ./hello
```

```text
% time     seconds  usecs/call     calls    errors syscall
------ ----------- ----------- --------- --------- ----------------
  0.00    0.000000           0         1           read
  0.00    0.000000           0         1           write
  0.00    0.000000           0         2           close
  0.00    0.000000           0         3           fstat
  0.00    0.000000           0         8           mmap
  0.00    0.000000           0         3           mprotect
  0.00    0.000000           0         3           brk
  0.00    0.000000           0         1         1 access
  0.00    0.000000           0         1           execve
  0.00    0.000000           0         1           arch_prctl
  0.00    0.000000           0         1           set_tid_address
  0.00    0.000000           0         2           openat
  0.00    0.000000           0         2           newfstatat
  0.00    0.000000           0         1           set_robust_list
  0.00    0.000000           0         1           prlimit64
  0.00    0.000000           0         2           getrandom
  0.00    0.000000           0         1           rseq
------ ----------- ----------- --------- --------- ----------------
100.00    0.000000           0        34         1 total
```

总数 34。刚才那 36 行里,形如 `名字(参数) = 返回值` 的有 35 条,`-c` 只收进去 34 次,没进表的只有 `exit_group(0) = ?`,它写在进程退场的那一刻。咱们真正想干的 `write` 只出现 1 次,占大头的是 `mmap` 8 次、`mprotect` 3 次、`brk` 3 次,`openat` 与 `newfstatat` 各 2 次。这些活是动态链接器在找 `libc`,再把它铺进进程的地址空间。`% time` 那一列里,末尾的 `total` 一行按定义是 `100.00`,其余各条调用在咱们这一趟里都是 `0.00`。这一列逐趟会抖,同一台机器上再跑几十趟,总有几趟某一格不是零。这点活太少,拿它谈性能谈不出东西来。

您写的是一行字,机器替您跟内核说了 34 句话。

## 用户态和内核态:同一条指令,谁说了算

程序要动一下,得跟内核说一声。咱们想想,为什么非得说这一声?因为机器把“能干什么”分成了两档,而程序默认待在低的那一档。

书里把这层界线写得很直白,咱们看它给的原文(CS:APP 2e,书页 715):处理器有一个 mode bit,置上时进程跑在 kernel mode,也叫 supervisor mode。清零时跑在 user mode,用户态里不许执行特权指令,停机、改 mode bit、发起一次 I/O 都算在内,书里的原话是“Any such attempt results in a fatal protection fault.”。想换到内核态,只有一条路,书里写的是“The only way for the process to change from user mode to kernel mode is via an exception such as an interrupt, a fault, or a trapping system call.”

这层界线的另一面,咱们在汇编那一卷里亲手见过:一条用户态的 `hlt` 送到机器上,进程当场吃到 `SIGSEGV`。它凭什么死、死在哪一层,那一卷交代过,咱们这里只留一句,用户态碰特权指令,机器不留情面。

反过来,合法的路也是有的。上面那屏里的 `write(1, "hello\n", 6) = 6` 就是咱们走通的一条路:程序在用户态喊一声,内核替它把六个字节写到 1 号,再交还一个 6 回来。

## 三张脸:系统调用、异常、中断

内核递给程序的东西很多,这一篇咱们只认三张脸,认脸的办法是看它们在屏幕上的样子。

系统调用的脸就是一行,`write(1, "hello\n", 6) = 6`。行里三样东西:谁(`write`)、什么参数(1 号、六个字节)、结果(6)。书里把它的来路写得很清楚:“The most important use of traps is to provide a procedure-like interface between user programs and the kernel known as a system call.”(CS:APP 2e,书页 707)。同一本书接着写,每个系统调用有一个唯一的整数编号,对应内核里一张跳转表上的一个位置(书页 710)。那一段讲的是 32 位机器的口径,指令名跟今天的 x86-64 不一样,咱们只要它给的两层:系统调用属于 trap,背后挂着编号和一张表。

异常的脸长在另一条路上。咱们写一个八行的程序,存成 `divzero.c`,除数是运行时才算出来的,源码里那个 0 是 `argc - 1` 的结果:

```c
#include <stdio.h>

int main(int argc, char **argv) {
    volatile int zero = argc - 1;   /* 运行时才算出是 0 */
    int x = 1 / zero;
    printf("%d\n", x);
    return 0;
}
```

```console
$ gcc -O0 -o divzero divzero.c
$ ./divzero; echo "exit=$?"
exit=136
```

shell 能看到的是 `exit=136`,咱们拆一下这个数:128 加 8,8 号信号就是 `SIGFPE`。把 trace 的尾巴拉出来看:

```console
$ strace -o div.trace ./divzero; tail -1 div.trace
+++ killed by SIGFPE +++
```

它上面还有一行,写着 `si_signo`、`si_code`、`si_addr` 三个字段,把 `tail -1` 换成 `tail -2` 就能连它一起看见。`si_code` 报的是 `FPE_INTDIV`,整数除零。`si_addr` 那一栏每次跑都不一样,咱们不把它印出来。异常是机器自己报上来的:程序算了一步不该算的除法,机器当场把这件事递出来,起因就在当前正在执行的指令。这一类就叫异常。

第三张脸不在程序里,在 `/proc` 底下。中断是定时的,咱们谁也躲不开,内核接一次就记一笔:

```console
$ grep TIMER /proc/softirqs
       TIMER:       8573      11865       7883       4636       9421       5136       9258       3695       8172       4317       7527       6976       6897       3587       6476       3416       8464       4596       7134       8328
```

笔者机器上 `nproc` 报出来是 20 颗 CPU,这行就有 20 个计数。过两秒再看同一行:

```console
$ sleep 2; grep TIMER /proc/softirqs
       TIMER:       8594      11868       7889       4638       9431       5146       9267       3700       8194       4323       7532       6983       6908       3597       6488       3417       8480       4604       7162       8338
```

每一栏都比刚才大,多的涨了 28,少的只涨了 1。这些数是本机此刻的快照,您那边一定不一样,稳的是它一直在涨。中断和前面两张脸的区别在这里:它不由哪条指令引起,是设备或者定时器从外面递进来的,程序那边什么都不知道。

三张脸看下来,内核干的活可以收成一句话:它是替咱们照看资源的那一层。拿刚才那个 hello 来说,它碰过的每一样都在 trace 里:

```console
$ strace -e trace=openat,read,mmap,mprotect,write,exit_group ./hello
openat(AT_FDCWD, "/etc/ld.so.cache", O_RDONLY|O_CLOEXEC) = 3
...
read(3, "\177ELF\2\1\1\3\0\0\0\0\0\0\0\0\3\0>\0\1\0\0\0\300y\2\0\0\0\0\0"..., 1024) = 1024
...
write(1, "hello\n", 6hello
)                  = 6
exit_group(0)                           = ?
+++ exited with 0 +++
```

文件在里面,`openat` 找到那个 `.so`,`read` 把它的头读进来。这一屏没有落文件,程序自己那句 `hello` 就插在 `write` 那一行中间,上面教的 `-o` 正是为这个。内存也在,`mmap` 一次次要地址空间,`mprotect` 一段段改权限,那几行里带着本机此刻的地址和长度,咱们略过去了,只看形状。CPU 时间也在,上面这些活全是内核替它跑的,它自己那点打印反而是最轻的一步。读进来的那个 `\177ELF`,咱们见过,工具链那一卷里读 ELF 文件头的时候,魔数就在文件最前面。资源管到这一层,往后那四象限就有得画了。

## 内核里放多少东西:宏内核和微内核

三张脸是任何内核都躲不开的活。咱们要问的,是一个很朴素的问题:这些活,有多少放进内核态?

一派人把整个操作系统都塞进去。xv6 那本书给了它名字和定义,咱们看原文,这样组织的叫宏内核,monolithic kernel,“the entire operating system consists of a single program running in supervisor mode.”(xv6,书页 23)。好处是子系统之间互相调用很便宜,它们本来就是同一个程序的几部分。代价是这棵树只会越长越大,而内核里的一个 bug“may cause the entire computer to crash”。

另一派人反过来,内核里只留绝对少的功能,大头拆成用户态的服务进程去跑。这是微内核,microkernel,书里的原话是“put an absolute minimum of functionality in the kernel itself”,以及“The bulk of the operating system runs as user-level server processes.”。咱们看它紧接着举的例子,文件系统就跑在用户态而不是内核态(xv6,书页 24)。代价是多了一层消息来往。哪种更好,那本书自己说没有定论:“there is no conclusive evidence one way or the other.”

现实里的阵营也不含糊。Linux 是宏内核,xv6 自己同样如此(“like most Unix operating systems”,xv6 书页 25)。Minix、L4、QNX 走微内核那一侧。这里要交代一句口径:xv6 那本书里的示例内核跑在 RISC-V 上,咱们手上是 x86-64,上面引的都是组织方式,跟指令集无关。

## Cinux 在谱系上的位置

说了半天的两派,该给咱们自己一个位置。只是下判断之前,把能摆的凭据摆出来,免得话说得比证据满。

名字的来历摆在那里:仓库自述里写着它要当“C/C++'s Linux”,一句大白话就是把 Linux 那套用 C/C++ 再写一遍。还有更具体的一条,系统调用号照着 Linux x86_64 的表排,表照人家的排,行为就有人家的参照,只有一处是咱们自己的安排,把一个号留给了自己的退出方式。再往后是路,目标是二进制接口也对齐 Linux,也就是常说的 Linux ABI 对齐,同一个调用号、同一套参数与返回,现成的程序才有机会直接接上来。

至于宏内核还是微内核哪一侧,判据得摆出来,不然说了也不算数。判据本身是个问句:内核的活跟子系统是编在一块儿、一起待在内核态,还是被拆成一堆用户态的服务进程各自跑?咱们这边眼下能直接指认的只有一条,内核只有一棵树,从小长到大,中间不换树。照这个问句去看,答案落在宏内核那一侧,跟 Linux、xv6 同侧。

## strace 被挡住的时候

咱们这一篇从头到尾都在用 `strace`,它有一处会被内核挡下来。

要是您想抓的是已经在跑的别人的进程,不是自己启动的程序,`strace -p` 会当场报错:

```console
$ sleep 300 & strace -p $!
strace: attach: ptrace(PTRACE_SEIZE, 124): Operation not permitted
strace: sysctl kernel.yama.ptrace_scope = 1 may be restricting this attach
```

上面那一趟里,`strace` 报完这两行就以退出码 1 收场。挡它的是内核里的 Yama 模块,这个模块管着一个叫 `ptrace_scope` 的设置,笔者机器上读出来是 1,意思是 ptrace 只许在父子之间用。报错行里的进程号是本机此刻的,您那边不是同一个,形状一样。strace 自己的报错里就把这个设置的名字点出来了,这一行是它递给咱们的线索,不用去别处翻。

处置很省事:要看自己的程序,咱们就用 `strace ./程序` 这个形状,父子关系,一路畅通,上面那些 trace 全是这么录下来的。

如果还想在一个几乎空的环境里试一遍,咱们也能做到,换一套自己的用户名字空间和 PID 名字空间就行。里面 `ps -e | wc -l` 数出来是 4,这里头还数进了表头和管道自己的几个进程。这个数是笔者那一趟的,您那边不会一样,`strace` 照跑:

```console
$ unshare -Urm --pid --fork --mount-proc sh -c "ps -e | wc -l; strace -o /dev/stdout /bin/echo hi" 2>&1 | head -6
4
...
openat(AT_FDCWD, "/etc/ld.so.cache", O_RDONLY|O_CLOEXEC) = 3
...
```

屏幕上剩下的 trace 行带着本机此刻的地址和长度,咱们没印出来。挡人的是那个设置,不是环境本身,换一套自己的名字空间,`strace` 照样跑。

## 双视角锚点:同一行 hello,两个人各看各的

**打印一行字。**应用程序员这边,`printf("hello\n")` 是一次库函数调用,一行代码,写完就觉得这事结束了。轮到内核作者,同一行字的底下是 34 次系统调用:找 libc、铺地址空间、读文件头、改权限,真正写出去的那一次只有 `write(1, "hello\n", 6) = 6`。您跟内核之间的每一次交流,它都要记一行。

**编译器和机器,谁说了算。**应用程序员把编译器当裁判,`1 / zero` 这样的写法编得过去,他自然觉得没事,一条 `hlt` 写进汇编源文件,`as` 也照收不误。轮到内核作者这一侧,咱们看到的就不一样了:编译器只负责把字翻译成指令,真正的裁决在机器执行到指令的时候。除零当场投 `SIGFPE`,用户态的 `hlt` 当场投 `SIGSEGV`。编译过了,离“能干”还差着一次执行。

**同样是看一眼机器,口径不一样。**应用程序员打开 `strace`,看到的是一堆不认识的函数名,他关心的只有程序有没有按预期跑完。轮到咱们看内核这一侧,同一屏数字换了个读法:哪几次是文件、哪几次是内存、哪几次是输出,哪些活是程序自己请的、哪些是它起步之前就欠下的。数字是本机此刻的,关系是稳的。

下一篇咱们把资源摆成四象限:进程、调度、内存、文件系统,各看一眼它长什么样。
