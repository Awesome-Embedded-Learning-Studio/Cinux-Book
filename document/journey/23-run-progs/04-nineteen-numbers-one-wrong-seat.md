---
title: "04 · 十九个号位,一把坐错的座"
description: "strace 四组抄单子,依赖面全过,缺口成片。时钟在宿主上一声不响,因为宿主有 vDSO,咱们没有,musl 只能敲真 syscall,时钟三件必做。十九个新号一次铺开,表界挪到三百一十九。ash 报 Function not implemented,探针打的是四号:stat 六个卷都坐在 lstat 的座上,busybox 头一个按真 ABI 敲的就是四号。实弹推进到 /bin/hello 全链通,卡点留给 1.2M 的大家伙。"
chapter: 23
order: 4
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - syscall
  - history
---

# 十九个号位,一把坐错的座

咱们动身之前,做了一轮侦察。咱们把客人请到宿主上,拿 strace 看它们敲的到底是哪些门。四组各跑一遍:最小的 echo 一组,带管道的 ls 一组,交互的 ash 一组,读钟的 date 一组。单子抄回来跟咱们的号位表一对,对出来的头一笔是好的:read、write、close、mmap、brk、mprotect、信号全家、fork、execve、wait4、getcwd、chdir、getdents、arch_prctl、gettid、futex,四卷的供给线应答如流,缺的一张也没有。缺的聚成了一片,咱们照着单子补。

侦察里还揪出一个险些漏掉的大头。四组的 strace 里,时钟家族的号一次都没出现:time、gettimeofday、clock_gettime,三件全都哑了。客人明明是要钟的,date 与 ls -l 一进门问的就是几点。它们不出声的原因在宿主:宿主的 Linux 有 vDSO,一块映射进用户世界的缓存页,读钟在那里面就办完了,根本绕开了内核,strace 自然也就看不见了。咱们的机器没有这层便宜,musl 只能退回去敲真的 syscall,咱们要是不把读钟的号立起来,这样的程序进门就卡死。vDSO 本身做不做、怎么讲,是另一笔教学上的事,真读钟的几个号是硬的。

咱们把号照单子铺开,十九个新号一次落了齐,一个号配的是一件文件。名目、号位与头一版的答法,咱们用一张表摆开:

| 号 | 名目 | 头一版的答法 |
|---|---|---|
| 5 | fstat | 走打开的座问后端 |
| 20 | writev | 散写的向量一次递进来,公共写路径从 write 里提出来两家共用 |
| 21 | access | 把路折好了递进门面,resolve 到了就答零 |
| 58 | vfork | 当普通 fork 办 |
| 63 | uname | 六个槽,一槽六十五字节,报 Cinux、cinux、0.3.0、x86_64,空槽摆零 |
| 72 | fcntl | 一律答读写态(假话,这一卷末段换真) |
| 96 | gettimeofday | 墙钟的秒与微秒 |
| 102/104/105/106/107/108 | getuid、getgid、setuid、setgid、geteuid、getegid | root 的答案恒是零,连 set 也答了个零 |
| 157 | prctl | 吞下不响(假话,这一卷末段换真) |
| 201 | time | 墙钟的秒数 |
| 228 | clock_gettime | 单调钟踩一百赫兹心跳,纳秒一栏凑的恒是整千万 |
| 231 | exit_group | 与 exit 走的是同一个门 |
| 257 | openat | 相对描述符开门,AT_FDCWD 的哨兵值转发给老门面,正经描述符号答 Enotdir |
| 262 | newfstatat | 同上,stat 版的姊妹 |
| 302 | prlimit64 | 老指针给读,答一对无穷,别人的 pid 拒(假话,这一卷末段换真) |
| 318 | getrandom | 拿心跳凑,异或上地址与一个金常数再搓散,一次给的量至多二百五十六字节(假话,当场挂号) |

表里有几处要配的上下文。writev 就是上一篇时滞疑案的头号嫌疑人,这一案到了实弹那一段才销。时钟三件的步子在表里写粗了:墙钟的起点问 RTC,主板上那块电池养着、关机也走的实时钟,只惰性地读一次。单调的步子踩的是一百赫兹的心跳,十毫秒一步的粗步,细表咱们后面再请。id 的六件是单用户的教学门面,完整的身份与权限语义排在后面的卷,这里立的是个不撒谎的零。fcntl、prctl、prlimit64 这三句头版的假话,加上挂了号的 getrandom,这一卷的末段统一换成真话。openat 与 newfstatat 旗标里的 CLOEXEC、NONBLOCK、DIRECTORY 几位,头一版吃了不认,后来的活里,CLOEXEC 与 NONBLOCK 两位落了实,DIRECTORY 还是不认的。表尾的 vfork 与 exit_group 不算进十九个新号的数,它们的门牌挂在老手艺上:vfork 当普通 fork 办。Linux 里 vfork 的语义是父进程挂着等,孩子紧接着 exec 换掉地址空间,musl 的 spawn 正是这么用它的,拿 fork 近似是立得住的,不挂父的细情留给教学叙事里慢慢说。表界从二百一十九挪到了三百一十九,把新住户都圈了进来,号照的仍是 Linux 的原样。

铺号的同时,咱们给分发表立了个探针:空号被敲的时候,打出的就是一行 unserved。本是诊断期的临时件,后来留下了。客人在哪扇门前吃了闭门羹,咱们在串口上一眼就看出来,ash 的报错也从此有了翻译。

然后是这一段真正的金矿,案子的起头,是 ash 报了一句 Function not implemented。咱们顺着探针一看,它敲的是四号,stat 的正号,而咱们的表上,四号是空的。stat 挂在了哪?六号。认字那一卷立 stat 的时候,咱们自己点过这笔:人家号表上六号是 lstat 的座,stat 坐的是四号。可自家的 shell 与自家的内核同错对齐,用户侧的枚举与内核的表是一个源头,错座上也回回是绿的,一绿绿了六个卷。busybox 是第一个按真 ABI 敲四号的客人,一敲就破了。咱们修得干脆:stat 挪回四号,六号立了 lstat,咱们没有符号链接,两个号挂的是同一个 handler,跑的是同一个语义。用户侧的枚举同源,自动跟着对齐了。

到了实弹推进,咱们报三笔。头一笔:writev 落了地,上一篇的时滞当场根治,ash 的输出实时了,这一案就此销了。第二笔记的是内置命令全通,echo 敲了就应。第三笔是外部命令的全链:ash 里递一条 vfork,fork 造了个孩子,execve 换了世界,程序跑完进了 exit_group,wait4 收了尾,/bin/hello 输出了原文,退出码一分不少地交割。卡点也跟着来了,而且是两桩连生:换 /bin/busybox 这个 1.2M 的大家伙,fork 出的孩子 exec 它,答的是 I/O error,hello 一万四千字节上下通了,busybox 一点二 M 败了,量的嫌疑大,嫌疑压在盘命令的跨度上限与块缓存的边界上。同时 ash 自己在每条外部命令之后段错误了,init 一遍遍地重拉它,拉起来还是崩了。两桩并成了一案,下一段咱们补一张全参的画像,补好了再去会它们。
