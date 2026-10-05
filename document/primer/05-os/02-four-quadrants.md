---
title: 02 · 四象限导览
---

# 02 · 四象限导览:同一台机器,四张脸

上一篇咱们把内核看成一个资源管家,它管的东西不止一样。这一篇不用新的概念把您埋起来,咱们只做一件事:在本机敲四条命令,每一张命令的输出都对应一类资源,也就是一张地图。

下面这一段是同一台机器的同一刻。咱们把四条命令连着跑,四张脸就摆在一屏里:

```text
$ ps -e -o pid,ppid,stat,comm
    PID    PPID STAT COMMAND
      1       0 S    bwrap
      2       1 S    bash
      6       2 S    bash
    212       6 R    ps

$ top -b -n1 -w 120 | head -5
top - 10:14:59 up 20 min,  1 user,  load average: 0.74, 0.58, 0.31
Tasks: 5 total, 1 running, 4 sleep, 0 d-sleep, 0 stopped, 0 zombie
%Cpu(s):  0.2 us,  0.0 sy,  0.0 ni, 99.8 id,  0.0 wa,  0.0 hi,  0.0 si,  0.0 st 
MiB Mem :  15828.7 total,  13132.2 free,   2169.5 used,    772.9 buff/cache     
MiB Swap:   4096.0 total,   4096.0 free,      0.0 used.  13659.1 avail Mem 

$ free -h
               total        used        free      shared  buff/cache   available
Mem:            15Gi       2.1Gi        12Gi       3.9Mi       772Mi        13Gi
Swap:          4.0Gi          0B       4.0Gi

$ lsblk -o NAME,SIZE,TYPE,FSTYPE,MOUNTPOINTS
NAME   SIZE TYPE FSTYPE MOUNTPOINTS
```

最后那一段底下,笔者只留下了表头,`lsblk` 报出来的设备行一行都没有搬。那些行里的挂载点就是本机此刻真实的挂载路径,`lsblk` 只是如实报出来,搬给您没有用处,咱们到文件系统那里换成能讲形状的写法。

笔者还得再交代三条:笔者手上这个环境是受限的,下面这些读数都是本机此刻的。它里面的 `ps -e` 只有四到六行,`/` 被挂成只读,块设备也就那么几块,`tty` 还会答一句 `not a tty`。您在自己手边的机器上跑同样一批命令,列名和形状都一样,数字差别会很大。

## 进程:内核手里的一张花名册

咱们来设想一个躺在硬盘上的程序,它自己不会跑。内核把它搬进内存,再分给它一份 CPU 时间,它才开始动。这一层抽象解决的事情很小:让机器上有许多个“正在跑的程序”同时存在。内核得随时为您答得出三个问题,谁在跑、是谁把它拉起来的、它现在什么状态。

这三问的答案有名字,叫进程。内核眼里的进程就是一条身份证,`ps` 那四列正好一问一答:`pid` 是我,`ppid` 是谁生了我,`stat` 是我在干嘛,`comm` 是我叫什么。这本册子全长七格,这里只取其中四格。

咱们自己搭一个场景,不管您的机器上有多少条进程,出来都是这个形状:

```text
$ sleep 300 & sleep 300 & sleep 300 & ps -e -o pid,ppid,stat,comm --forest
    PID    PPID STAT COMMAND
      1       0 S    bwrap
      2       1 S     \_ bash
      6       2 S         \_ bash
    137       6 S             \_ sleep
    138       6 S             \_ sleep
    139       6 S             \_ sleep
    141       6 R             \_ ps
```

三条 `sleep` 是咱们自己叫起来的,`--forest` 只是把 `ppid` 那一列画成了一棵树。屏幕左边那排缩进来自 `ps` 自己,退一格进一层,谁挂在谁底下,顺着看就清楚了。同一件事在 `/proc` 底下还有另一个面,您看 `ps` 那一屏和 `/proc/<pid>/status` 里的一行行字段,读的是同一份内核记录。

```text
$ grep -E '^(Name|State|Pid|PPid|Threads|VmSize|VmRSS)' /proc/137/status
Name:	sleep
State:	S (sleeping)
Pid:	137
PPid:	6
Threads:	1
```

这块屏里笔者摘掉了 `VmSize` 和 `VmRSS` 两行,那是本机此刻的具体数字,内存那边另有更值得看的现场。屏里那个 `137` 是笔者那一趟的 `sleep`,您换成自己的进程号就行。

`STAT` 这个字母有一张官方对照表,`R` 在跑或者在排队,`S` 在睡,`D` 在等 IO,`T` 被停住了。`Z` 是这一列里最容易让咱们误会的一个:孩子退场了,父进程还没收尸,那一行就还挂在表上,连名字都改成了 `zombie <defunct>`,父进程一收尸它就没了。

线程在花名册里的位置,咱们这一篇只记一句。同一个进程里的两个线程,是 `clone3` 这个系统调用开出来的,同一个进程里两条 `clone3` 就是两个线程。它们在调用里共享了地址空间与文件描述符,又各自拿着自己的栈。本机这一版 C 库走的是 `clone3`,别的机器上可能是它的同门 `clone`,名字不是这句话的重点。

要往深里走,操作系统教科书里讲进程的那一章就是入口,把进程状态、上下文切换怎么发生一层层铺开。至于“一个进程从生到死经历了什么”,那是后面那一卷里专门有一篇的事,这会儿只给您地图。

## 调度:一个 CPU,一群等着上场的

`top` 的头几行就是咱们到调度这一步要看的全部现场。第一行报的是开机时长和三个 `load average`,第二行按状态数了一遍任务,第三行的 `%Cpu(s)` 列着 CPU 时间去了哪里。

问题出在很多人第一次看这几行的时候,`%Cpu(s)` 后面那个 `us` 和每个进程的 `%CPU` 对不上。本机 20 个 CPU,咱们起四个纯用户态忙循环,`%Cpu(s)` 的一格只到 `20.2 us`,而 `ps` 里那四条各自报着 99.3 到 99.6。

```console
$ for i in 1 2 3 4; do bash -c "while :; do :; done" & done
$ ps -e -o pid,stat,pcpu,comm --sort=-pcpu | head -6
    PID STAT %CPU COMMAND
    154 R    99.6 bash
    155 R    99.6 bash
    156 R    99.6 bash
    153 R    99.3 bash
      6 S     0.1 bash
```

咱们忙起来之后取到的 `%Cpu(s)` 是这样一格:

```text
%Cpu(s): 20.2 us,  0.2 sy,  0.0 ni, 79.1 id,  0.0 wa,  0.0 hi,  0.5 si,  0.0 st 
```

两个百分比都对,咱们得看分母。`%Cpu(s)` 拿整机所有 CPU 当分母,四条忙循环占掉 20 个里面的 4 个,正好两成。每进程的 `%CPU` 拿一个核当分母,四条忙循环各自占着的核都被吃满了,所以报 99 出头。上面报出的两个数都是本机此刻的值,稳的只有“整机口径”这件事。

`load average` 那一行后面写着三个数,咱们还得再泼一盆冷水。手册交代得清楚,它数的是在跑的(状态 `R`)加上等 IO 的(状态 `D`)调度实体,而且是 1、5、15 分钟的平均。咱们把本机三拍的读数挨着排一遍,空闲的时候、四个忙循环刚起来、以及三秒之后:

```text
0.53 0.53 0.29 1/608 149
0.80 0.59 0.31 5/612 162
0.80 0.59 0.31 1/608 167
```

从空闲到忙起,头一格从 `0.53` 挪到了 `0.80`。三秒过去再看,它还是 `0.80`,一格都没动。机器已经忙起来了,读数还在慢慢跟上,这就是平均值的滞后。收工以后它同样会挂在上面好一会儿。拿它当秒表用,您会失望。

那调度器具体在忙什么,咱们看它手上这段时间:只有一个 CPU 的一段时间,想要跑的任务排着队,挑谁上。挑谁,教科书里叫策略,怎么把人换下去、怎么把现场存起来,叫机制。两个词在操作系统教材的调度章里是分开讲的,咱们往后会分别遇到。

## 内存:说好给您,和真给您

`free` 那一屏看着像一台机器的自述,其实它只是几张纸上的算式。咱们翻手册就能看到,四列各自对到 `/proc/meminfo` 的一个字段:`total` 是 `MemTotal`,`free` 是 `MemFree`,`used` 是 `MemTotal - MemAvailable`,`buff/cache` 是 `Buffers+Cached+SReclaimable`。

咱们把同一刻的 `/proc/meminfo` 按上面四条式子算一遍,再跟 `free -k` 那一行对着看:

```console
$ free -k
               total        used        free      shared  buff/cache   available
Mem:        16208548     2255468    13413600        3960      791396    13953080
Swap:        4194304           0     4194304
$ awk '/^MemTotal:/{t=$2} /^MemFree:/{f=$2} /^MemAvailable:/{a=$2} /^Buffers:/{b=$2} /^Cached:/{c=$2} /^SReclaimable:/{s=$2} END{printf "total=%d free=%d used=%d buff/cache=%d available=%d\n", t, f, t-a, b+c+s, a}' /proc/meminfo
total=16208548 free=13413276 used=2255792 buff/cache=791396 available=13952756
```

`total` 与 `buff/cache` 一字不差,咱们算出来的 `free`、`used`、`available` 各差 324 kB。差的不是公式,是两条命令之间内存又动了一下。

咱们把镜头往下挪一拍,看一个开口要 256 MiB 的程序。造件短得可以当场抄完,存成 `memdemo.c`:

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    const size_t kBytes = 256u * 1024 * 1024;   /* 256 MiB */
    char *p = malloc(kBytes);

    memset(p, 1, 4096);                          /* 只碰第一页 */
    printf("pid=%d touch=1page\n", (int)getpid());
    fflush(stdout);
    sleep(2);

    memset(p, 2, kBytes);                        /* 每一页都碰一遍 */
    printf("pid=%d touch=all\n", (int)getpid());
    fflush(stdout);
    sleep(2);
    return 0;
}
```

它落地、起来、然后咱们在它睡的那两秒里取数:

```console
$ gcc -O0 -o memdemo memdemo.c
$ ./memdemo > memdemo.log &
$ ps -o pid,vsz,rss,comm -p <pid>
$ grep -E '^(VmSize|VmRSS|VmPeak)' /proc/<pid>/status
$ grep -E '^(MemFree|MemAvailable):' /proc/meminfo
```

`<pid>` 换成 `memdemo.log` 里那个进程号。咱们在这一刻去看它,两个数字摆在一起会很刺眼。一组是只碰过第一页的样子,另一组是把每一页都碰过之后的样子:

```text
    PID    VSZ   RSS COMMAND
    172 265012  1876 memdemo
VmSize:	  265012 kB
VmRSS:	    1876 kB
MemFree:        13427460 kB
MemAvailable:   13966944 kB
```

每块快照里笔者摘掉了一行 `VmPeak`,它在原屏上是表头之后的第二行(全块的第三行),和 `VmSize` 是同一个数。`MemFree` 与 `MemAvailable` 那两行是同一刻从 `/proc/meminfo` 取的,接在 `VmRSS` 后面。

```text
    PID    VSZ   RSS COMMAND
    172 265012 264016 memdemo
VmSize:	  265012 kB
VmRSS:	  264016 kB
MemFree:        13190468 kB
MemAvailable:   13729984 kB
```

`VSZ` 一个 kB 都没动,`RSS` 从 1876 涨到 264016。地址空间是内核答应给您的那一片,常驻内存是您真的碰到、物理页真的落到手上的那一部分。说好给的和真给的,可以差出两个数量级。

咱们把镜头挪到整机那边,这一拍也看得见。256 MiB 被真碰过之后,`MemFree` 从 `13427460 kB` 掉到 `13190468 kB`,少了 236 992 kB,大约 231 MiB。两个数都是本机此刻的值,关系才是稳的:虚拟那块一说好就涨,常驻那块要碰到才涨。

要往深处走,页表、缺页、`/proc/<pid>/maps` 您都可以去操作系统教科书的地址空间那一章找。这一卷只把这层地图交到您手上。

## 文件系统:一块设备,一条路径

同一块存储,咱们有另外几条命令可以从几个面看它。`lsblk` 给块设备,`df -h` 给用了多少、还剩多少,`findmnt` 与 `/proc/mounts` 给挂载点与挂载选项。后两个是同一份内容的两种排版,`findmnt` 是排得更整齐的一种。

文件系统是进程和地址空间之后的第三个抽象。文件是一个字节数组,目录是一串配对,每一对里放着一个人能读的名字和一个编号。您写的程序只认路径和字节,底下那块设备叫什么都不用管。

有一件事第一次看 `lsblk` 的人多半会以为输出打歪了:同一个设备底下,挂载点会竖着排好几行。那不是格式问题,咱们碰到的一个文件系统确实可以有好几条路径可达。手册解释了 `lsblk` 为什么要分成 `MOUNTPOINT`(只给一个)与 `MOUNTPOINTS`(多行单元格给出全部)。

```text
$ lsblk
NAME MAJ:MIN RM   SIZE RO TYPE MOUNTPOINTS
```

另外还有三个地方值得多看一眼,咱们都只点一下:

- `lsblk` 默认只报块设备的形状。加上 `-f`,每一行还会带上文件系统类型、版本、UUID、可用空间与占用百分比,一眼能看出哪个设备上铺着 ext4、哪个是 swap。没挂载的那几行,类型和版本还在,后面几格是空的。
- `df -h` 后面跟上一条路径,它只会报落在那个文件系统上的容量:总共有多少、用了多少、还剩多少,单位换成人读的 G。
- `findmnt` 不加参数就打印整棵挂载树,给它一个路径就只报对应的那一行,`-o` 能自己挑要哪几列。

这几样东西往深里走,在操作系统教科书里占着一整章。文件怎么在一块设备上定下来,目录树怎么组织,一次打开到底做了什么,那一章会挨个讲。这一卷咱们只到挂载这一层,再往里那一步留给它。

### 隔三秒再看一眼:哪些稳,哪些飘

上面这些命令读的都是状态,状态会动。咱们把其中几条隔三秒再跑一遍,对着看:

```console
$ cat /proc/loadavg
0.74 0.58 0.31 3/608 190
$ sleep 3; cat /proc/loadavg
0.74 0.58 0.31 1/609 201
```

咱们跟开头那一屏的头一行比,三个数一个都没动,末尾那一段从 `3/608` 跳到了 `1/609`,最后那个数从 `190` 跳到了 `201`。同一批命令里,`lsblk` 报出来的东西一个字符都没变,`Tasks: 5 total` 这一行也没变。

飘的是 `MemFree` 这一列、`used` 这一列、`available` 这一列,还有 `loadavg` 末尾那一段。稳的也别当成永远稳:这几条命令里没有一条造出新进程,进程条数这一拍才没变。咱们稳下来看形状:哪一列对应哪个概念,谁是谁的子进程,虚拟与常驻怎么分。

## 双视角锚点:我的程序在跑,和我要 256 MB

同一件事,咱们让坐在桌子两边的人分头来看。

**应用程序员那一边**,“我的程序在跑”就是字面意思。您启动它,它占着您分到的时间片,您跟它说话,它答话。要内存就跟系统要一块,拿回来就能写,写进去的东西就在那儿。至于机器上还有谁在跑、内核把您这个进程排在第几位、您那块地址对应哪一页物理内存,都不在您的视野里。

**轮到写内核的人**,同一句话要翻成两本记录。花名册是第一本,`ps` 报出来的那几列就是点名用的。这里有一件最容易忘的事,进程退场和销号是两回事,`Z` 那一行就是这么来的。花名册的条数、状态、父子关系,全得您维护。

内存那两笔由另一本记录着。程序说“我要 256 MB”,内核答应他的是地址空间,`VSZ` 那一刻就涨上去。可物理页要等他真的碰到才给,`RSS` 才跟着动,整机那边的 `MemFree` 也是那一刻才开始掉。答应和交付分开记,是这一层能撑住“每个进程都以为自己独占内存”那个错觉的原因。写内核的人要是把这两本合成一本,过量的承诺就会在最不该的时候变成缺页和杀死进程。

咱们到这儿看的是每条任务各自怎么往前走。要是好几条任务同时伸手去改同一份数据,上面这些花名册和内存记录还兜得住吗?咱们把这个问题带到下一篇去读一份最小的多线程程序。
