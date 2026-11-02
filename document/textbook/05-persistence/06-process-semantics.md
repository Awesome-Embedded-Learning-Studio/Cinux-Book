---
title: 06 · Unix 进程语义全景:生的清单,换的手续,收尸的名册
---

# 06 · Unix 进程语义全景:生的清单,换的手续,收尸的名册

咱们走到第 5 部的第六章,名下押着的欠条也就攒到了该清的时候。5.1 把 fd 表的认亲章程押给了今天:孩子出生了,他的表从哪来,他拿到的号能不能保住,dup 与 dup2 的椅子几时落座,这些都归在第一节与第三节的名下。5.5 押下的三票也在今天兑,一票问的是线程住进同一副身子之后信号该给谁,一票问的是开机名单里那些查不到父亲的条目,还有一票是保号案的正戏,5.5 只放了引子,正戏咱们今天唱了。4.4 在条件等待的卷尾押的也有三票:join 与死亡侧的那一票,clone 与线程身份的这一票,今天咱们一并兑了,futex 的门面名目,咱们收尸的时候顺手交齐。前台组的席位与键路,5.5 已经讲过了,今天补的是组员怎么进组、散了场归谁来收。

再往回勾的是两笔旧事。2.4 搓好的任务骨架,当年有的只是让与抢的循环,生与死的大事都还没动工,那一年办的生与退,是内核自己摆任务、收尾的活,本章办的是系统调用这一层的生与死,今天咱们动的就是他们。3.3 的许诺簿里,写时复制早就给今天的主角留了座,那一章您已经见过他,正活今天轮到他来唱了。所以这两笔今天接上头:fork 生孩子用的正是 2.4 的骨架,抄家(这词儿 1.4 就立过了,抄的是对方的地址空间)抄的正是 3.3 的许诺簿。

本章的活,咱们按一辈子的次序数下来五样。头一样是出生的日子,fork 把一个任务原样抄成了两个。第二样是分身的戏,clone 商量着分了家,线程与 vfork 都在分家的戏码里。第三样是换台的活,execve 在旁边搭好新的世界,然后一脚踩了进去。第四样是离场与收尸的事,exit 留下带着退出词的僵尸,退出词是 4.5 立过名的收据,人怎么退的场就记在词里,收尸的柜台是 wait4。末一样是没爹的孩子,讲孤儿的收养,还有替 1 号看住位子的那个内核任务。五样走完了,您在 shell 里敲下的每一行命令,从回车到下一行提示符之间的全部手脚,就都能一一对上号了。

量法还是老的三条,码打的头。实况锚是 r35_gui_fin 的名字,与 5.3 用的是同一口锅,行号的写法一律是文件名加冒号再加行号,全按他的数来。5.1、5.2、5.4、5.5 数的是 r34_compile_self,两边的数字对不上是正常的事,您别跨章去抄。下文写的都是短名,内核文件住的是 kernel/,用户程序住的是 user/,都是仓里的真实路径。

书要翻三本的,各有各的用处。头一本是《Unix 环境高级编程》的第三版(英文的全名是 Advanced Programming in the UNIX Environment),这一回是教科书层的头一次,咱们请他出场,下文里咱们就叫 APUE,他是进程控制那一章的正典,咱们的主战场在第 8 章:书页 229 的两份返回与写时复制,书页 237 的僵尸与孤儿,书页 249 的换台。第二本还是 TLPI 的那本,vfork 的原话住在书页 523 与 524,线程组的来历在书页 604,他是 Linux 视角的详尽一手。第三本就是 xv6 了,fork、exec、wait 的三件套在第 1 章,书页 11 与 12 起的头,书页 53 认了他自己的整抄,书页 85 讲的是收养,他的篇幅最小,也就最便于您对照。引文咱们都逐字核过了,书里跨行断开的词拼回了整词,截断的地方补的是咱们的话,原句您拿着去核,准还是不准您说了算。

屏幕上的事,咱们如实交代:本章就没有屏。进程语义的戏不出画面,这些全落在返回值、状态字与名册的进出上,咱们誊不出,所以也不硬造,能誊的只有码,跑的活留给练一练。

动身之前的话,咱们把全景在一张表里铺开,正戏走的还是五幕,各归各的系统调用:

| 幕 | 谁上场 | 一句话 |
|---|---|---|
| 出生 | fork | 抄一个自己,一口两回 |
| 分身 | clone 与 vfork | 按旗分家,线程与押票都在这 |
| 换台 | execve | 旁边搭新世界,搭好才换 |
| 离场与收尸 | exit、exit_group 与 wait4 | 存下退出词变僵尸,柜台领走名册摘人 |
| 没爹的孩子 | detach_offspring | 父亲走得早,孩子归一号 |

> **前台组的席位与键路,5.5 已经讲完了,今天补的是组员怎么进组、散了场归谁来收。您换个日常的问法:您在 shell 里敲一行命令,您按下回车之后,从这行字到下一行的提示符之间,机器都干了些什么?程序是从哪来的,跑完了之后他去了哪,shell 又凭什么知道该打下一行提示符了?**
>
> 底下是一场四步的戏。shell 的头一件活是 fork,把自己的身子原样抄成一个孩子。孩子上台走的是 execve,换上程序自己的世界。程序干完了活就 exit,身后留下一具带着退出词的僵尸。shell 在 wait4 里领了尸,拿到了退出词,这就打下了一行提示符。前台组管的是席位。标准盘是咱们一直构建的带 busybox 的启动盘,盘上的 ash 是带着作业控制的,依据在 busybox 的构建配置里(busybox.config 的 :1160 立了 CONFIG_ASH_JOB_CONTROL,文件的住处在 user/rootfs/busybox/ 下),每条命令的孩子落生就被 setpgid 进自己新开的组,前台随 tcsetpgrp 切到孩子的组,收了工再切回。终端的键找得到他,靠的是 tty 一侧的前台门,console 把 TIOCGPGRP 与 TIOCSPGRP 两问答了,写进来的组号照单全收,键与信号都投给前台门记着的那一组。本章分作五节来点亮:生的一场,分身的一场,换台的一场,离场与收尸的一场,没爹的孩子的一场,父亲走得早的孩子去哪,咱们末节见。

## 生一个一模一样的:fork

fork 干的事一句话就说完:把自己原样地抄成一个孩子。动手之前的话,咱们把一个疑问摆平:要跑个新程序的话,直接装不就行了吗,为什么还要把旧的原样抄一份?因为 fork 把两件事分成了两截,一截是造一个能跑程序的地方,另一截管的是决定跑哪个程序。shell 抄出了一个自己,孩子的号、孩子的组、孩子继承来的 fd 表就都有了着落,然后才轮到 execve 的换台。两截分清楚了,中间就容得下种种的安排,这是 Unix 给进程语义定的头一笔。APUE 的第 8 章从 fork 讲起,咱们也从他讲起。这对返回值的来历,咱们借他在书页 229 留下的原话来看:`This function is called once but returns twice. The only difference in the returns is that the return value in the child is 0, whereas the return value in the parent is the process ID of the new child`。调用只发生了一次,返回却落在了两处,父亲这边拿回的是孩子的号,孩子那边拿到的是零。零也不是随手给的,同一页的下一句给了理由:`The reason fork returns 0 to the child is that a process can have only a single parent, and the child can always call getppid to obtain the process ID of its parent`。您看,孩子要想知道自己的父亲,问一句就知道了,所以父亲那边的号才值得占住返回值的位子。

咱们看自己家的。fork 的处理体在 sys_fork.cpp,建造的手艺全在 fork.cpp 的 BuildForkChild,咱们从 :59 起往下走查。头一件大事是地址空间的抄法,孩子不抄父亲的页,两边共享的其实是同一批物理页,谁动了笔谁就触发缺页,笔落到了哪页就补抄哪页,这正是 3.3 的一记写时复制,fork.cpp 的 :71 到 :72,接话的一行是 CloneSharedSpace。APUE 在同一页就交了底:`Modern implementations don't perform a complete copy of the parent's data, stack, and heap, since a fork is often followed by an exec. Instead, a technique called copy-on-write (COW) is used`。fork 的后头往往跟着 exec,整锅端地抄一遍纯属浪费,笔真落到了纸上再抄才是正办。xv6 在书页 53 也认了他自己走的是整份现抄:`One way to implement this is to copy the entire memory of the parent to newly allocated physical memory for the child`,后半句他还补上了,说 xv6 自己就是这么干的。整份的物理内存现抄,xv6 图的就是直白,咱们把直白换成了缺页处的一记补抄。

接着往下数的话,咱们看孩子带走的家当,一项一项都是有据的。虚拟地址区间的登记原样搬一条过来(:77)。内核栈是新发的四页,由 Pmm 按阶发了页(:81),栈为什么非新发不可?2.4 的身家单子上有答案,上下文切换切的就是栈,一根栈两个人用的话,谁切了谁就散。父亲的整张 SyscallFrame 抄到了新栈顶(:99 到 :102),寄存器的影像与浮点的影像随栈走(:62 到 :65 与 :92)。信号的动作表与屏蔽集一次循环原样搬过来(:110 到 :113)。进程组与会话的号保持原样(:114 到 :115)。工作目录、影像名字、控制终端、限额、uid 与 gid 的这一串全家都跟着搬(:116 到 :127),您留意,孩子的 exe 此刻还写着父亲的影像,这名字要等的是 exec,procfs 里的 comm(ps 报的进程名)就打这来。凭证与限额跟着走是有讲究的,孩子能开父亲开得了的文件,能占父亲占着的限额,越权的门则一扇都没有多开。

fd 表的家在 :128,认亲由 FileTable::clone() 一手办:一把锁内拍下整表的快照,逐档直接摆进了新表,close-on-exec 的位跟着一起搬(file_table.cpp 的 :271 到 :289,file_table.hpp 的 :226 到 :227 原话写着 one locked snapshot,说的是一张表、一把锁、一次抄完的生意)。里头的每个号都原位不动,读写偏移跟着底下的文件走,而不是跟着表走,每一档对底下的开档多记一个主人,后走的那个才放。5.1 那时把 fork 继承记在了 adopt 名下,如今照码走的是整表快照的路子,adopt 家的椅子咱们一把都没请,他们伺候的是别的客,底下讲交接手续的时候咱们就有请。

### fd 的交接余款:dup、dup2 与管道

fork 之外的话,号在一张表里头还有几样交接的手续,5.1 预告过的全套交接,就在这儿开演了。dup 与 dup2 两个号的处理体住在 sys_pipe.cpp,跟 pipe 是一家的。dup 拿的是 adopt 找座,从零起挑最低的空位,把旧户口挪来坐(sys_pipe.cpp 的 :89 到 :90,file_table.hpp 的 :117 是那把椅子的脸)。dup2 由您指定号,duplicate_at 是换座不换人的,原坐在那个号上的户口被顶下来(sys_pipe.cpp 的 :98 到 :99)。fcntl 的 F_DUPFD 走的是 adopt_from,带着您给的下限找座,您说从十号起,他就照您说的从十号起挑,F_DUPFD_CLOEXEC 走的也是同一条路,顺手把新座上的 close-on-exec 位也立了,依据在 sys_fcntl.cpp 的 :88 到 :89。5.5 交代过 ash 的行编辑开工,拿 F_DUPFD_CLOEXEC 从十号起找座存自己的终端描述符,八座的表一伸手就摸到了满,咱们把表添到十六座,那是当年的旧案,如今的 fd 软限已是一〇二四,那一笔的下半截,座怎么找、位怎么立,今天咱们交齐。

保号的案子也在这儿销。5.5 押过一笔:家当继承头一版走的是找座的路,父亲表里的十号,到了孩子表里被折成了三号,后来的复盘判回了保号,父亲的几号就是孩子的几号。判回的底气就是开头拍下的整表快照,号与档是原位对应的,5.1 认亲章程里保号的那一半,正戏在今天唱完了。管道咱们一句带过:pipe 的两端是当场开的两份新户口,读端写端各找了一个空号入座,旗上只认 close-on-exec 与非阻塞两样(sys_pipe.cpp 的 :43 到 :51)。

抄完了有一项咱们偏偏不带:信号的待办集,孩子出生时是两手空空的。全仓没有一处给孩子的待办位赋过值,task.hpp 的 :157 到 :160 段设计注把章程写死了,fork 抄的是动作与屏蔽,exec 三样是全清的,待办不在抄的单子上。

> **您停一下,想想看:动作表抄了,屏蔽集也抄了,为什么偏偏待办集不带?带过去会坏什么事?**
>
> 待办位记的是“信号已经送到读者门口,还没人开门”,门口是出生前的读者。孩子是另一位读者,旧传票记到他的头上没有道理,何况传票里压着的可能是一发 SIGKILL,传票带着走的话,孩子还没跑第一句就得出局了。动作与屏蔽就是这个人面对信号的家法,家法是传得下去的,案子是不能跟着传的。

回到两份返回值的收尾。父亲那一路是好办的,HandleFork 回的是孩子的号,InvokeSyscall 把他写进了 rax,这手在 syscall_dispatch.cpp 的 :314 到 :315。孩子这一路不在 C++ 的地界里,孩子被拉起的时候,ctx.rip 直指的是一段汇编小桩 ForkChildEntry,它住在 syscall.S 的 :54 起,装回帧寄存器的活是它干的,末尾的三行在 :74 到 :76,咱们看:

```asm
    movq (%rsp), %rsp
    xorq %rax, %rax
    sysretq
```

这里给的是节选,上头十七行进出门与重装寄存器的活咱们略去了。“孩子得零”的零就是这么来的,靠的是一记异或,顺着 sysretq 就直接带回了用户态。

还有一问您多半想问:fork 回来,父子俩谁抢得早?sys_fork.cpp 的 :15 给了答案的一半,孩子入座的时候就是 runnable,父亲写完号就回来了,轮到谁要看调度器的安排,章程里没有长幼的讲究。所以 fork 之后别赌次序,两边各自跑各自的,没有谁等着谁的事,您要等孩子,柜台开的是 wait4。

fork 的闸还有一道。guard_spawn_depth(fork.cpp 的 :49 到 :55)看的是生者自己的内核栈,用到四分之三的时候就拒生。机理 fork.cpp 的 :45 到 :48 注释里写得明白:生孩子的调用链必须稳稳坐在将要交出去的那根栈内,拼装孩子时摸到的帧,是不能跑到栈底外头去的。这道闸兜的是另一路人:深调用链里再 fork 的生者,他的栈眼看要见底,闸在他动手前就把他拦下了。fork 炸弹归的就不是这道闸,炸弹的孩子拿的是新发的四页栈,就在浅栈上接着生它的孩子,闸对浅栈是不响的,炸弹死于内存、任务块与名册的耗尽,那是另外的几桩事。失败的路数咱们也分两样看:空间、区间、栈页哪样领不着,BuildForkChild 回的是空,号回 -kEnomem(sys_fork.cpp 的 :12 到 :14)。深度闸超限走的却是 base::safety::Check,失败即内核停机(assert.hpp 的 :59 到 :66,noreturn 的 AssertionFailed),机器就停在了那儿,不是一个负号回得去的。孩子头一次进门之前的话,FinishForkChild(:138 到 :147)把孩子的号写进他自己世界里的 set_child_tid 字,这笔字要是拷不过去的话,孩子就带着 SIGSEGV 退场了。出生这场戏到这儿齐了,孩子要是走的不是原样抄、而是照单子分家的路,那是下一节 clone 与 vfork 的戏。

## 商量着分家:clone 与 vfork

fork 走的是原样抄,clone 走的是照着单子商量抄哪些。处理体 HandleClone 把旗折成了一张 ClonePlan,建造与分派就此分了家,计划书递给了 clone.cpp。单子上认的是四面旗:kCloneVm(Linux 的 CLONE_VM)的值是 0x100,认的是同一副地址空间。kCloneFiles(Linux 的 CLONE_FILES)的值是 0x400,认的是同一张 fd 表。kCloneThread(Linux 的 CLONE_THREAD)的值是 0x10000,认的是进同一个线程组。kCloneSetTls(Linux 的 CLONE_SETTLS)的值是 0x80000,认的是 fs 基址按来客给的定。四面旗的落法,咱们在 clone.cpp 的 :24 到 :36 一眼看穿:

```cpp
    if ((plan.flags & syscall::kCloneVm) != 0) {
        kBorn->space     = kParent->space;
        kBorn->vmas      = kParent->vmas;
        kBorn->user_root = kParent->user_root;
    }
    if ((plan.flags & syscall::kCloneFiles) != 0) {
        kBorn->files = kParent->files;
    }
    if ((plan.flags & syscall::kCloneSetTls) != 0) {
        kBorn->ctx.fs_base = plan.tls;
    }
    auto& child_frame    = *cinux::base::PtrAt<syscall::SyscallFrame>(kBorn->ctx.rsp);
    child_frame.user_rsp = plan.user_stack;
```

走查这一段的话,有一笔是要放在头里点的:四面旗里 kCloneThread 的落座不在这一段,他的活办在入座之后,sys_clone.cpp 的 :65 到 :67 才把孩子的 tgid 改指组长,组的事后头当面看。剩下的有两笔值得您停下来看。头一笔看的是地址空间,BuildCloneChild 在 :20 无条件跑了 BuildForkChild,您就算要 CLONE_VM,也照样付一遍写时复制的钱,然后再整锅换成共享的,活是能跑通的,钱却是一分不省的,这是把 fork 的路走熟了再分家的写法。第二笔看的是用户栈,末行把用户栈强按成了 plan.user_stack,抄来的帧上写的还是父亲的栈,新线程自己的栈全凭这一按,来客给零的话就沿用帧上的旧数(sys_clone.cpp 的 :53)。

四面旗怎么点?咱们点的是一整桌,没有单点的点法。一个能用的线程,要的是同一副地址空间、同一张 fd 表、同一个线程组,外加上自己的 fs 基址,四旗齐立了,再带 tid 三旗里的一两把。threaddemo(咱们写的线程演示程序)点的正是这样一整桌:0x100、0x400、0x10000、0x80000,外加让孩子自写 tid 与死时清字的两把。pthread_create 在 Linux 上点的也是同一桌,您对着它的源码数一遍就知道了,线程库底下调的其实就是 clone。

分家分到了同一副身家,号的口径就得分成两截来看。入座的时候 tgid 定成了 pid(scheduler.cpp 的 :114),CLONE_THREAD 的旗一立起来,孩子的 tgid 就改指组长(sys_clone.cpp 的 :65 到 :67)。于是 getpid 答的是 tgid,gettid 答的是 pid,组长的两个号是同一个,组员的两个号就分岔了。TLPI 在书页 604 交了来历,开头的 “Threads groups” 是原书的印法,咱们照录:`Threads groups were introduced in Linux 2.4 to allow threading libraries to support the POSIX threads requirement that all of the threads in a process share a single process ID (i.e., getpid() in each of the threads should return the same value)`(原书如此)POSIX 要求的是同进程的线程 getpid 报同号,Linux 添了线程组来满足它,咱们照办了。组员的 parent 指向的是造他的那个任务(fork.cpp 的 :109),组员 getppid 答的是组长的号,组长自己的 getppid 答的才是 shell,两家的父并不同人,这一层与 Linux 的差距,对照的一节给开了单。

5.5 押下的半张票在这里还上:组员收到了信号,该给谁?4.5 教过的 SendToGroup 是逐任务对 pgid 的,组里每人各立各的待办位,谁回程谁就来取(signals.cpp 的 :204 到 :210)。Linux 的款是组里挑一位来消费,两款都是许可内的做法,咱们挑的是人人都得自己看一眼的款,当时欠下的对照,现在就补齐了。

kill 这一侧的口径顺手交底:kill 递正数的 pid,按号认的恰好是一个任务,认线程不认组(sys_kill.cpp 的 :29 到 :32),您把组长的 pid 递过去,落在谁的头上就只有谁应,不会散给整组的。要成片地发的话,零与负数的 pid 是 kill 自己扫的,自带的 for_each_task 把整张名册挨个对过去,零对着自己的组,负数对着 pgid 为其绝对值的组(sys_kill.cpp 的 :45 到 :50)。SendToGroup 那一手让给了终端前台门,console 与 pty 的前台门从键里翻出信号,投给前台门记着的那一组(console_node.cpp 的 :77 与 :100,pty_node.cpp 的 :87 与 :228)。真要单点一个线程的话,tkill 与 tgkill 两个号是真的件不是壳,按 tid 认的是人,tgkill 还要您多递一道 tgid,对不上的就回 -kEsrch。

vfork 最有意思了。58 号的字面处理就是 HandleFork,不带任何的押票(syscall_dispatch.cpp 的 :165),真 vfork 押在 clone 的 kCloneVfork 旗上,他是 Linux 的 CLONE_VFORK,值定的是 0x4000。父亲在栈上造的是一枚 Completion,把指针存进了孩子的 vfork_done,自己睡了进去(sys_clone.cpp 的 :77 到 :84),孩子的出口要么是 exec 要么是 exit,才把父亲放了出来。TLPI 在书页 523 对这门手艺给咱们留了原话:`Execution of the parent process is suspended until the child has performed an exec() or _exit()`。

押票是值当的,理由也在同期书页:省下一整套地址空间的抄写,孩子马上就要换新的世界,抄了也是白抄。约束也在同一页立了三条,孩子在放行之前不能跑自己的事,不能动父亲的空间,也不能从包裹他的那个库函数里 return,三条守不住的话,下场是没个准的。书页 524 的旧闻也有一句:`Some UNIX systems do indeed implement vfork() as a call to fork(), and Linux also did this in kernel 2.0 and earlier`。咱们的 58 号走的正是这一路,您想押真票的话,请从 clone 的旗进来。

clone 的孩子怎么入座,这里头有讲究:入座的时候,孩子是 blocked 的(sys_clone.cpp 的 :64),父亲把 tid 字写完了、把押票上好了,最后一记 unblock 放了行,孩子醒来的时候,父亲的写笔早就收工了。顺路交底的有两笔。clone 旗低字节的退场信号栏,咱们只验不用(sys_clone.cpp 的 :30 到 :33),验过了就丢,孩子退场时永远发的是 SIGCHLD。tid 的三旗,0x100000 的 CLONE_PARENT_SETTID 交的是父亲写 tid 的活,0x200000 的 CLONE_CHILD_CLEAR_TID 交的是死时清字的活,0x1000000 的 CLONE_CHILD_SETTID 交的是孩子自写 tid 的活,依据在 sys_clone.cpp 的 :61 到 :75,threaddemo 一跑您就知道了,咱们练一练里见。clone3(435 号)也挂在了号表上,处理体直通的是 -kEnosys,这个入口占了一个名,里子却是空的。

## 旁边搭新世界:execve

fork 抄的是旧世界,execve 造的是新的世界。APUE 书页 249 的说法是:`When a process calls one of the exec functions, that process is completely replaced by the new program, and the new program starts executing at its main function`。您看,exec 造的不是新进程,换的只是程序,pid 是跨换台不改的,现役进程的正文、数据、堆与栈,从盘上换成一套全新的,人还是原来的人。xv6 书页 12 补了另一半:`When exec succeeds, it does not return to the calling program`,后半句他也留下了,说装入文件的指令从 ELF 头声明的入口开跑。成功的 exec 没有回头路,调用的行永远等不到回话。

Cinux 的 HandleExecve 在 sys_exec.cpp 的 :243 起,咱们按动手的次序走。折好用户的路径,找着了节点,读头 64 KiB 的内容来认脸(:244 到 :265),64 KiB 足够 ELF 头与程序头表住下了。然后是关键的一步,:266 到 :271 造出全新的 AddressSpace 与 VmaStore,新世界是在旁边搭的,当前的世界原封不动,搭砸了随时能全身而退。接着数的是 argv 与 envp,逐串抄进内核的暂存,串数是拿 kMaxArgumentStrings 封的顶,字节折在按 RLIMIT_STACK 折出来的预算里,超了的路一律回 -kE2big。空 argv 的时候,咱们补一句 argv[0] 当路径(:108 到 :215)。ELF 的认脸与逐段映射交给 loader(:288 到 :291),镜像带着动态解释器的,入口换成了解释器的头一条指令,ELF 自己的入口让给装载器去请(:313 到 :319)。进场画把 argc、argv、envp、auxv(auxv 是内核附给新程序的一串说明条目)写上了新栈(:358 到 :375),AT_RANDOM 的字节取自硬件的随机源,AT_EXECFN 指着的是路径,新程序一睁眼就知道自己是怎么被叫进来的,栈顶带了一层随机滑距,信号蹦床页的映射也在此时办了。都齐了,才走到换台的点,声明的节选如下,身份改名的一笔与四行注解省了(:376 到 :395):

```cpp
    kSelf->signals.reset();
    kSelf->clear_child_tid = 0;
    kSelf->files->close_on_exec();
    base::Shared<mm::AddressSpace> old_space  = std::move(kSelf->space);
    base::Shared<mm::VmaStore>     old_ranges = std::move(kSelf->vmas);
    kSelf->space                              = std::move(fresh);
    kSelf->vmas                               = std::move(ranges);
    kSelf->user_root                          = kSelf->space->root();
    kSelf->space->activate();
    auto& mutable_frame    = const_cast<SyscallFrame&>(frame);
    mutable_frame          = {};
    mutable_frame.user_rsp = kEntryRsp;
    mutable_frame.rcx      = kFirstInstruction;
    mutable_frame.r11      = kUserFlags;
    mutable_frame.rax      = 0;
```

“换台”两个字的全部手续就都在段里了,您对着码数就知道。信号的三样全清,clear_child_tid 归了零,close-on-exec 的档挨个收割。旧世界挪进了 old_space 与 old_ranges 两个把手,押到函数出栈的时候才拆,新世界换了上去,activate 装的是 CR3。末了整张帧清了零,rcx 对准了新程序的头一条指令,execve 的“返回 0”落进的是全新世界的入口,旧的调用行永远收不到话。段的末尾还压着一笔债:孩子若是 vfork 生的,换台换到了这儿,睡着的父亲也就跟着放出来了(:396 到 :400)。exec 与 exit 是放行仅有的两处,第二节押的票,这里兑现了一半。

换台点之前的话,每一条错误路都发生在旧世界还映着的时候,号回的是负的 errno,程序接着跑它的,咱们要的就是这个退路。教学壳(2.3 章陪咱们亲手下过单的)的孩子 exec 碰壁,靠的就是它,喊一声 exec refused 再 Exit(1)(shell.cpp 的 :296 到 :298,练一练您自己去读)。顺路交底的是 loader 的口径:认不出的镜像回 -kEnomem,-kEnoexec 全仓可达的只有一条路,零程序头的却是脚本式解释器镜像(:308 到 :310),两个号您别互相猜。

哪些过得了 exec,哪些过不了,咱们两边点清:

| 跨 exec 活下来的 | 当场清掉的 |
|---|---|
| 号、组、会话、父亲 | 信号的动作表、屏蔽集与待办 |
| 工作目录、控制终端 | clear_child_tid |
| 限额、凭证 | close-on-exec 的档 |
| exe 与 comm 换新 | |

fd 表的保号案就在表里:号本身跨 exec 全保,咱们收走的只有立了 close-on-exec 旗的那几把。

## 离场与收尸:僵尸和 wait4

exit 拿的是 60 号,exit_group 拿的是 231 号,处理体是同一个(sys_exit.cpp 的 :9 到 :12),头一步就把码抬进了退出词,编码的事 4.5 教过,咱们引过即走。交底的话有一笔:两个号咱们不作区分,exit_group 不去通知别的线程,组里各走各的门。

exit_current 在 scheduler.cpp 的 :317 起,咱们数出来的次序是一板一眼的。有 vfork 押票的话就放行父亲(:321 到 :325)。ClearTidWord 清掉用户内存里的 join 词(join 是线程会合用的那个字,等的人守着它睡),叫醒了等着的一位(:326,futex.cpp 的 :108 到 :124)。fd 表放掉了(:327)。退出词存了档(:332)。然后孤儿的安顿归 detach_offspring(:333)。最后才轮到僵尸的裁决,给的是节选(:334 到 :348):

```cpp
        const bool kThreadMember = kPrev->pid != kPrev->tgid;
        if (kPrev->parent != nullptr && !kThreadMember) {
            kPrev->state = TaskState::kZombie;
            SendSignal(*kPrev->parent, cinux::syscall::kSigChld);
            if (waiters_.remove(*kPrev->parent)) {
                unblock(*kPrev->parent);
            }
        } else {
            tasks_.remove(*kPrev);
            if (kPrev->parent != nullptr) {
                offspring_.remove(*kPrev);
            }
            kPrev->state         = TaskState::kDead;
            local().pending_reap = kPrev;
        }
```

裁决的两岔,看的是有父与否,再看的是 pid 与 tgid 同不同,咱们一岔一岔看。有父的正编组员(pid 与 tgid 相同、没进线程组的),改挂的是僵尸态,给父亲发了 SIGCHLD,父亲在睡等的话就把他叫醒。咱们不另设停尸房,僵尸的身子还挂在名册与孩子名册的两张表上,直到有人来领了才摘。线程组员走的是另一岔,直接从两张名册摘了出去,人停在切换之后的回收队里。APUE 在书页 237 给僵尸下了定义:`In UNIX System terminology, a process that has terminated, but whose parent has not yet waited for it, is called a zombie`。名字听着是骇人的,职责却是单纯的,人已经走了,退出词还在他的身上,等的就是父亲来读。

名册的事也在这里接全了,咱们一态一态点过去。2.4 那年的任务只有四态,如今的 TaskState 挂了六态:ready 与 running 是让与抢的老两位,blocked 是等东西的,stopped 是被信号摁住的,kZombie 在 4.5 的死讯一节照过面,今天唱的是正戏,kDead 管的是收尾。procfs 侧把六态折成了字母来报,前两位报的都是 R,blocked 报的是 S,stopped 报的是 T,僵尸与死报的都是 Z。死任务的回收另有一道,栈与任务块要等切换过去了之后才拆(reap_pending,scheduler.cpp 的 :570 到 :595),2.4 说过的是死后切换完才收。

> **您停一下,咱们想想看:SIGCHLD 的默认处置是忽略,父亲多半没装任何的动作,睡在 wait4 里的父亲凭什么醒?信号的这一路之上,哪一段还在使着劲?**
>
> 醒靠的不是信号派发,靠的是名册。exit_current 发完了 SIGCHLD,紧跟着的就是一记 waiters_.remove 加 unblock,把睡在 wait_child 里的父亲直接拎醒。默认忽略的家法是不装的,通报是照走的,走的是调度器自己的唤醒线。父亲要是设了动作的话,信号就照样地投。没设的话,名册的这一线也保证他醒。

收尸的柜台是 wait4(61 号),您递过去一个 pid,他认的是四种形:正数指定的是这个孩子,负一指的是任意孩子,零指的是自己进程组里的孩子,再负的数指的是 pgid 为其绝对值的组(scheduler.cpp 的 :363 到 :386)。选项只看的是 WNOHANG 一位,细节后头给咱们交底,处理体的行数不多,咱们整段看(kernel/syscall/sys_wait.cpp 的 :19 到 :33,入口的状态指针探子省了):

```cpp
    proc::ChildReceipt           receipt{};
    if (!proc::Scheduler::self().wait_child(kWanted, kNoHang, receipt)) {
        return -kEchild;
    }
    if (receipt.pid == 0) {
        return 0;
    }
    if (frame.rsi != 0) {
        const int kStatus = static_cast<int>(receipt.code);
        if (!uaccess::CopyToUser(frame.rsi, &kStatus, sizeof(kStatus))) {
            return -kEfault;
        }
    }
    return static_cast<long long>(receipt.pid);
```

三路的出口都在码上:wait_child 扫不着对得上号的孩子,就回了 -kEchild。WNOHANG 之下有活孩子没僵尸的时候,receipt.pid 拿到的是零,柜台答的也是零。领到了尸,状态字给用户的是一份裸拷(:26 到 :31),状态字里装的就是退出词,验算的口径给您:八到十五位装的是码,零到六位装的是杀号,第七位是永远不立的,core-dump 的位全仓都没有。pid 的四种形与 4.5 教过的 kill 四种读法是同一套方言,您对着记一遍就牢。交底的有三笔:rusage 指针被无声地略过,getrusage 整个调用回的是 -kEnosys,WUNTRACED 与 WCONTINUED 咱们收下不理,停着的孩子不在上报的名单里。也在场的还有 waitid(247 号),只伺候 P_PID、P_PGID、P_ALL 的三种,写一封清零的 siginfo 回来。

wait_child 的睡法咱们也交代一句:他自带的是重试循环,持着队列的锁扫僵尸,扫得着的就当场领走,扫不着而又有活孩子的话,就把自己推上了 waiters_,挂成了 blocked,醒来的时候再扫一遍,假醒了也不怕(scheduler.cpp 的 :430 到 :462)。挂成 blocked 的手法与 4.4 的同步家什是同一个底子,park 完了换人,醒了就重试,一家手艺使在了两处。顺路的交底有一笔:睡法里没有一处看的是信号,SendSignal 碰上 blocked 的号,立的也只是待办位(signals.cpp 的 :236),父亲的醒与不醒,凭的是退场一侧的那记 unblock,信号在这门睡法里是不当门铃的。线程组员为什么绕开这一套?threaddemo 的最后一问就是答案,对线程的 tid 调 wait4,回的是 -kEchild,组员不入可收尸的册。join 的等法在 tid 字上,人死了 ClearTidWord 清字叫醒等着的人,退出码走的是共享内存。两套是两套的等法,两路是两路的兵马,互不串对方的门。

futex 的门面名目,咱们顺手在这儿交齐,4.4 押过的票今天就兑。号嘛,是 202(syscall.hpp 的 :91),操作嘛,咱们认两码,您递零是来睡的,递一就是来叫人的,您随号带来的私有位 128,咱们收下就摘(sys_futex.cpp 的 :10 到 :22)。地址没对齐的话,两路咱们都回 -kEinval,词要是读不了,回的就是 -kEfault,睡的一路词要是对不上,咱们当场回 -kEagain,对上了才交出 CPU,叫的一路答给您的是叫醒了几位,操作码认不出的话,咱们回的就是 -kEnosys。里子咱们在 4.4 的《一个词,二百五十六只桶》里教过了,今天咱们只挂门牌。

柜台的最后一问,咱们留给下一节:要是父亲始终不来领的话,甚至父亲走得早呢?

## 没爹的孩子:孤儿的下落

孩子还没走的,父亲就走在头里了,孩子就成了孤儿。APUE 书页 237 把章程说全了:`What normally happens is that whenever a process terminates, the kernel goes through all active processes to see whether the terminating process is the parent of any process that still exists. If so, the parent process ID of the surviving process is changed to be 1 (the process ID of init). This way, we're guaranteed that every process has a parent`。xv6 在书页 85 也有同款的一段:`If the parent exits before the child, the parent gives the child to the init process, which perpetually calls wait`,后半句咱们也替他说了,每个孩子就都有父亲来善后了。

孤儿怎么安顿,咱们有自己的章程。detach_offspring(scheduler.cpp 的 :398 到 :428)顺着死者留下的孩子名册挨个去办,收养人的挑选交给 find_adopter,九行的码,咱们整段读(:388 到 :396):

```cpp
Task* Scheduler::find_adopter(const Task& dying) {
    for (Task* task = tasks_.head(); task != nullptr; task = tasks_.after(*task)) {
        if (task != &dying && task->pid == 1 && task->space.holds() &&
            task->state != TaskState::kZombie && task->state != TaskState::kDead) {
            return task;
        }
    }
    return nullptr;
}
```

三条标准各有各的用意,咱们挨条看:pid 为 1 保证的是收养人唯一又长命,有用户世界保证的是他真在跑程序的位子上,非僵尸非死保证的是他还能收别人的尸。收养落笔改的就是父亲指针,被收养的孩子若已是僵尸,收养人得的是一份 SIGCHLD 加一记 unblock,没爹的僵尸归他收。找不到收养人的话,孩子摘出了名册,父亲指针清了空,无父的僵尸当场收尸,游魂是不留的。

时序上孤儿的戏排出来是两出。一出是孩子走在了头里,父亲还在的时候:僵尸挂着等领,父亲随后也走了,过继的手续就轮到 detach_offspring 来办,收尸的活由收养人接手。另一出是父亲走在了头里,孩子还活着的:detach_offspring 当场改了他的父亲指针,孩子照常跑他的,日后他自己退场的时候,僵尸直接挂到了收养人名下。两头咱们都走到了,APUE 说的是 every process has a parent,在咱们家才算句实话。

组随组员的归属,咱们也在这里对上一句。组长名下是没有组的,组住在每个组员的 pgid 格里,是全组共记的一个号,组长退了场,号还挂在活着的组员身上,组就还是活着的,同会话里后来的人 setpgid 还进得来,咱们的 setpgid 立的只是这一层讲究。组真正散的时候,散在最后一个组员退场的那一刻。线程那头的组员另有一说,他们是永不变僵尸的,退场的时候当场摘了名,收尸的名册不给他们排座,第二节交过了底,咱们在这里同台再记一笔。

5.5 交过底的查不到父亲的条目,现在咱们能认领了:他是 kernel_init,内核自己的一条任务,1 号的位子他不去坐,父亲一栏是空的,getppid 问到他的时候,从来是不出号的。他的活计在 init_thread.cpp 的 :283 到 :321,挂了卷,开了控制台泵(键盘进来的字节靠它源源滚进行纪律),把盘上的 init 程序交给调度器的 seat 入座,坐的是 1 号,这一手带着 initial_process 的旗,号强制为一(scheduler.cpp 的 :103 到 :110),然后自己就退到了幕后,一条 wait_child(-1) 的循环永远守着,1 号退了场,他就把 wait_child 拿回的状态字解开来读,认出是几种死法里的哪一种,再入座新的一任。守夜的 kernel_init 不领养,他是让 1 号永远有人坐的人。

壳的事也在这交底。标准盘上的 /sbin/init 是 busybox,inittab 里一行 respawn 拉的并不是 ash,盘上 /etc/inittab 的 :3 原话是 console::respawn:/bin/su - cinux,拉的是 su,ash 是 su 登录之后落到的 shell,users.txt 的 :1 给 cinux 配的登录 shell 是 /bin/sh,buildroot 的 /bin/sh 就是 ash,两跳才走到您的提示符。提示符打 cinux> 的教学壳,只有不带 busybox 的兜底盘才请得出来,寻常的开机遇不见他。咱们读他的码,跑的活交给 ash,两头是不误的。

> **笔者在名册上记过一跤。当年给 wait4 整理孩子名单的时候,拿了个临时链表,用的是链接成员的默认形状,而孩子名册走的是另一条链接,临时表压入弹出的操作,把等待队列的链接也带得改写了。当时一台上跑着的是四个辈分的任务,个个都是有名又有等待的,链就被剪断了,僵尸出生的时候,waiters_ 里找不到等他的父亲,wait4 就一觉不醒了。offspring_ 与 waiters_ 两条链同住的是一份名册,各用各的链接成员,这是从一跤里摔出来的章程:拿临时容器整理链的话,链接成员必须照原链的来,借了默认形状,剪的就是别的链**。

## 同一出戏,两家的演法

xv6 的 fork 在书页 11,照面给的就是整份抄:`fork gives the new process an exact copy of the calling process's memory: fork copies the instructions, data, and stack of the calling process into the new process's memory`。书页 53 他也认了没有写时复制,咱们把抄写押给了缺页,xv6 押给了简单。wait 一款是两边同义的,书页 12 的一句写得分明:`If the caller has no children, wait immediately returns -1`,对上咱们的 -kEchild。shell 的四步也是同款,同页还有白描的一段:`Then it calls fork, which creates a copy of the shell process. The parent calls wait, while the child runs the command`。收养的那一出,书页 85 的 init perpetually calls wait 与 kernel_init 的守夜循环神似,差别在 xv6 的 init 自己就是 1 号,咱们把守夜人与 1 号分了家。

Linux 的那一边,TLPI 书页 604 的线程组咱们照办了,vfork 的押票押在 clone 旗上,路数是同源的,waitpid 与 wait4 也是一家的,咱们只有后者,包装的活交给了 glibc。差的几笔如实开单:wait4 的 rusage 不伺候,getrusage 回的是 -kEnosys,exit_group 与 exit 是不作区分的,clone 的旗谱只伺候四面加 tid 三旗,退场信号是只验不用的,组员 getppid 的口径也与 Linux 两样,咱们答的是组长的号,那边的 CLONE_THREAD 让组员与组长同答真父。进程组的这一侧也有一处照办一处没照,setpgid 动别人的孩子要求同会话,setsid 遇上已是组长的就拒,这些与 Linux 是同款的,而 Linux 规定 exec 之后孩子不许再改组,咱们没立的是这一层,组的约束停在组长死了组还活着为止。差距记在了明处,补不补的、几时补,后头的活说了算。

还有一笔三家都不一样:身家的管法。xv6 的 proc 是手写的数组加链,清场全凭的是手工。咱们 Task 的世界与 fd 表握在 base::Shared 手里,主人计数归了零才放,exec 换台的时候,旧世界挪进的是两个旧把手,押到函数出了栈才拆。fd 表的 clone 是一把锁下的整表快照,第一节引过的原话说的是同一件事。同一份的 Unix 语义,C++ 的家什让收尾的活轻了不少。

> **顺带交代的是一桩查案的手艺,跟上一框是同一个案子的。当年排这个毛病的时候,shell 的 Wait4(-1) 一直好使,差点凭它断定了名册没病。后来想明白了,对照组同时改了两个变量,shell 那头只有父子两代的层数,出事的现场是四代同堂,任务层数是根本没对齐的。打那以后笔者给自己换了章法,对照实验一次只动的变量只有一个,动了两个变量的数不作数。单拓扑的测试跑得再干净也不背书,测试里注入的假切换走不到真链,缺的正是多代同堂这一层**。

## 本章留下了什么

五幕连了起来,您手里就有 Unix 进程语义的全景了。fork 给的是一口两回,抄家靠的是写时复制,fd 表整表认亲保的是号,待办集是不带的。clone 按旗分了家,线程组让 getpid 有了两个口径,vfork 的父亲睡到孩子换台。execve 在旁边搭的是新世界,搭成了才换台,失败了全身而退,fd 只收 close-on-exec 的几把。exit 存下退出词变的是僵尸,SIGCHLD 与名册两条线把父亲叫醒了,wait4 的柜台凭四种 pid 形收尸。父亲走得早的话,孩子归的就是 1 号,无主的僵尸是当场收的,替 1 号看住位子的 kernel_init 永远候着。

| 术语 | 一句话 |
|---|---|
| 两份返回值 | 一次调用两次返回,父得孩子的号,孩子得汇编里一记异或出的零 |
| 写时复制 | 生时不抄页,共读同页,动笔才补抄,fork.cpp 一行 CloneSharedSpace 接上 3.3 的许诺簿 |
| 待办集 | 已到门口未开门的信号,出生不带,换台全清 |
| tgid 与 tid | getpid 答 tgid,gettid 答 pid,组员的两个号分岔 |
| vfork 押票 | 父亲睡在 Completion 上,孩子 exec 或 exit 才放行 |
| 进场画 | argc、argv、envp、auxv 写上新栈,新程序起身的行李 |
| 换台点 | 清信号、清 join 词、收 close-on-exec、换世界装 CR3、帧指到新入口 |
| 僵尸 | 退了场还没人领的进程,退出词在身,仍挂两张名册 |
| 收尸 | wait4 领走僵尸的退出词,把他从两张名册摘除 |
| 孤儿与收养 | 父亲走得早,find_adopter 把孩子过继给 1 号,无收养人当场收尸 |
| 深度闸 | 生者自己的内核栈用到四分之三就拒生,超限即停机,兜的是深调用链里的 fork |
| 整表快照 | fd 表的 clone 一把锁下整张换新,号与偏移原地不动 |
| join 词 | CLONE_CHILD_CLEAR_TID 指的字,人死清零叫醒等着的人,线程的会合走这里 |

没讲的咱们如实交底:futex 的门面名目,后头的章开。孤儿进程组与散场的那声 SIGHUP,本章是一概没碰的。execve 遇上多线程的进程该怎么清算,咱们没立章程,这一层记在了差距单上。Linux 那边定了 exec 之后不许改组,咱们没立的正是它,对照的一节已经开了单。

## 练一练

老样子了,您把猜放在头里,跑了之后您再对照。盘上的程序都是真的,猜错了不丢人,猜错了才记得住。两个演示程序都装在盘上 /bin 的下面,ash 里您敲全路径就行。

第一题跑的是 /bin/threaddemo,跑之前您猜五问。main 自报的 pid 与 tid,同号还是岔号?两行的线程自报里,tid 与 getpid 呢?main 开 /etc/motd 拿到的号,worker 那边读的是不是同一个?clone 答回的 a 与 b,和随后打出的 tid 词里的两个数,是不是同一对?两行自报末尾的 fs:0,打出的两个数对得上源码里哪两个字面量?跑完了,对照您的五问。再看末段:对线程的 tid 调的是 wait4,答的是几?

第二题跑的是 /bin/sigdemo,您猜四问。SIGSTOP 发了出去,孩子的滴答停不停?停了之后,WNOHANG 的 wait 答的是孩子的 pid 还是零?SIGKILL 发给一个负的数,收信号的都有谁?程序开头的 kill(child, 0) 探子答了几,一发真信号发出去没有?跑完了,您再对照。

程序免了,第三题跑的是壳。您在 ash 里敲 exit,提示符没了,您猜屏幕接下来会不会再有提示符?猜完了就试。再试的还有一件:敲一个不存在的程序,ash 死没死?然后有两处翻给您对照:盘上的 /etc/inittab,respawn 那行拉的是谁,ash 又是从哪一跳来的。再翻 kernel/init/init_thread.cpp 的 :283 到 :321,看守夜的循环里 wait_child 等的又是谁。最后读 user/programs/shell.cpp 的 :283 到 :309,您数一数,run 一条命令走了几步,跟本章的四步对上号。

第四题的程序也免了,读的是码。您翻 kernel/proc/task.hpp 的 :85 到 :168,数一数一个 Task 身上与本章直接挂钩的字段都有哪几个,再对照 2.4 里只管让与抢的骨架,看进程语义给任务添了多少件随身的家当。

### 参考答案

往下看之前的话,您把自己的答案落成字。

第一题的谜底咱们一样一样对,源码住的是 user/programs/threaddemo.cpp,想对码的随时翻。main 的 pid 与 tid 同号,组长的位子是他的。两行的线程自报里 tid 与 getpid 岔开了,组员的两个口径当场现形。motd 的号是同一个,而且是 0、1、2 之外的第一个空位,worker 读的正是父亲开在共享表上的号,CLONE_FILES 的共享是活的。clone 答回的与 tid 词里落的是同一对数,CLONE_CHILD_SETTID 押的那个字,由孩子头一次进门时自己写(syscall.S 的 :58 调 FinishForkChild,fork.cpp 的 :138 到 :147 办的正是这笔字,task.hpp 的 :168 注释原话写的是 `written in the child's own world before first return`)。这一回的 main 是在两次 clone 返回之后立时读词的,读到同数是这回调度赶上了,不是旗给的承诺,时序上读到零是完全可能的。fs:0 打出的两个数,对得上源码里 g_tls_a 与 g_tls_b 装的数,各自的基址按进去,读出来就分毫不差了。joined 两行里 tid 词已经清了零,退出码 65 与 34 摆在了明处。对 tid 调 wait4 答的是负十,组员不入可收尸的册,join 归的是 join,wait 归的是 wait。

第二题的谜底咱们一问一问对。滴答当场就静默了。WNOHANG 的 wait 答零:孩子停着,死是没有死的,也没有可领的僵尸,停态与僵尸是两个码上的事,wait 只认的是后一码。负数的 pid 是负的 pgid,SIGKILL 落给了整组。探子答的是零,孩子是活着的,发出去的真信号是一发也没有。wait 来收尸的时候,杀号一栏打的是 9。

第三题的谜底咱们一层一层看。提示符自己回来了。respawn 拉的是 su,ash 是 su 登录之后落到的 shell。ash 死了,上一跳的 su 也跟着退,respawn 就把这两跳再拉了一遍。敲的是不存在的程序,ash 是不死的,execve 的失败落在孩子身上,ash 自己就接着读下一行了。内核侧 kernel_init 的循环守的是同一个局,wait_child 等到 1 号退场的时候,就把拿回的状态字解开来读,再入座新的,busybox init 守的是盘上的局,kernel_init 走的是同一个路数。shell.cpp 的 run 走四步:Fork 抄一个孩子,孩子 Execve 换了世界,换不成的话喊一声 exec refused 再 Exit(1),父亲这一手的答案是 Wait4(-1) 领尸报号,正是本章四步的活样板。

第四题的谜底咱们一件一件数。号是有四样的:pid、tgid、pgid、sid。押票的名字叫 vfork_done,数出来的是一枚。席位给的是 controlling_tty,数出来的是一个。信号是三件套的:动作、屏蔽、待办。外加的是 clear_child_tid 的 join 词。2.4 的骨架管让与抢,今天这一章管的是生老病死,看的是同一个 Task,咱们把两章看下来之后,看到的厚度不一样。

## 延伸

- APUE 第 9 章 Process Relationships:会话、作业控制与守护进程的脱离手续都在这一章讲全,您想深挖前台组的来路,从第 9 章的门口进。
- TLPI 第 28 章:同一片进程地的另一份地图,与本章对照着读,口径更牢。
- xv6 第 1 章:fork、exec、wait 三件套在一节里讲完,书页 11 与 12 起,收养在书页 85,回头对照,三家的详略自己比出来。
- 去处的开单:进程语义收束了之后,照目录的分法,VFS 的座位在 5.7,socket 排在后头。futex 的门面名目,本章收尸那一节已经交齐。
