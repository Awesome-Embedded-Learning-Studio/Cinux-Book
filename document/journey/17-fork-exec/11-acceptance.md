---
title: "11 · 验收:会生会收的机器"
description: "面板安静,只改一行:shell 从卷上来,实跑读数 size 49DF00、pmm 31275,kernel.bin 95488 字节。测试 52 走到 56:宿主 44 件加机内 12 台,新套件四份——elf、cow、loader、shared,test_sched 添十案。实弹会话逐字:run /bin/hello,问候贴着 [3] exited with 42,exit 之后 init 重拉 shell。三块砖再对一遍数,第一幕收束。"
chapter: 17
order: 11
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - process
  - acceptance
---

# 验收:会生会收的机器

咱们收卷照旧看三样:面板、测试、实弹。这一卷咱们从面板看起,它出奇地安静——新行一行没有。安静是有道理的:fork、execve、写时复制,全是运行时的事,开机的面板上看不出花。动的只有一行,kernel_init 那行的尾巴。咱们实跑了一趟,把有数的几行也抄了上来:

```text
[kern] kernel at 200000 size 49DF00 entry 200040
[kern] pmm: 31275 pages free (probe 116000 ok)
[kern] boot task (pid 0) parks here, kernel_init (pid 1) takes over
[kern] kernel_init: root mounted, launching shell from the volume
cinux shell - type 'help'
```

in ring 3 换成了 from the volume。shell 不再从镜像里被扶下楼了,它成了一份文件,从卷上被装载起来的。面板的数,咱们拿实跑的和认字那一卷的对一对:size 从 9CE60 走到了 49DF00,差出的一截按页凑整是 1025 页。pmm 从 32300 走到了 31275,少的页数恰好是 1025——影像的跨度长了多少,开机常住的物理页就多出多少,一来一回对上了。kernel.bin 报的是另一个口径:文件的个头落在 95488 字节,认字那一卷收在了 91264,文件本身只长了四千二百多,装载器的新目录、fork 的克隆和一整卷的家族簿记都在这四千多里。面板的 size 量的是装机后内存影像的跨度(bss 也在内),kernel.bin 量的是落进文件的字节,两个数各量各的。

rtc 那行照旧每趟开机的数都不同,咱们不抄数。ahci 报的 8192 扇区没动,盘还是那一卷的 4 MiB,只是里头多了 /bin。还有一行是面板上看不见、只在运行时出现的:

```text
[kern] kernel_init: shell exited with 0, respawning
```

init 等到 shell 退了场,报了数,重拉一个新的。这一行您在下面的会话里会亲眼见到。

测试的家底,52 走到了 56:宿主 44 件,机内的 12 台。新到的套件四份,咱们逐份点。头一份是 elf 的三案,它是在宿主上跑的,考的是词汇件:良构的头解析出入口和段表,七种坏头逐个地拒:魔数、类、序、机型、类型、截断、段表越界,一家子全认齐了。第三案考的是读越界,越界的读取回一声干干净净的拒。cow 一份三案:有伴的写赚一张私页,末位的写原地复权,不受理的案子原样退回。loader 的一案也落了地,机内独跑:段按页落好了位,bss 的出身是零,帧数的收支相抵。shared 的一份数出六案,引用计数的两案,加 Weak 的四案——收官那篇说过的两位,测试在这儿到岗了。老套件里添案子:test_sched 添了十案,咱们挨个数:wait 收尸带退出码,死在 wait 之前的孩子照样答,无子的案子直拒 ECHILD,等着收尸的生灭循环页数回平,父死在前孩子改了无父,两家族各跑各的,再加 fork 的四案:克隆的镜像逐页对,单侧的写分家,入了座的子退出进父的 wait,子的帧与文件册全检。self_list 添一案:同一个对象骑的是两条链,互不腐蚀——三案连破里第三案修法的回归。test_pmm 添的相邻帧独立计数一案,fork 本体那篇数过了,咱们在这里记名而不重数。宿主那边 test_scheduler 同步改了名,pid 的词汇两侧对齐。四条构建线的读数咱们散文报全:普通线和 checks 线各 56,地址排查的 asan 线和数据竞争的 tsan 线各 44,后两条跑的只是宿主。

实弹的会话,咱们把串口原文逐字端上来:

```text
cinux> ls /
./
../
lost+found/
etc/
bin/
hello.txt
cinux> run /bin/hello
hello from the volume[3] exited with 42
cinux> exit
[kern] kernel_init: shell exited with 0, respawning
cinux shell - type 'help'
cinux> 
```

这一屏咱们念三遍。头一遍看 /bin:ls 的清单上多了一行目录,shell 和 hello 都住进了里面,盘成了程序的家。第二遍看的是 run:一行命令底下,fork 发生了,孩子的 pid 是 3——boot 是 0,kernel_init 占的是 1,shell 占的是 2,机器的第四个进程,是您刚才敲出来的。问候后面紧贴着的是 [3] exited with 42,中间没有换行的踪影,咱们数过字节:问候那串字长 22,hello 写出去的是 21,换行是没有出门的,咱们当小案一桩记,如实地转写,不做拦链路的处理。42 是 hello 自己 Exit(42) 交的码,父亲 wait4 收到了,原样报了数。第三遍看的是 exit:shell 退场,kernel_init 的 respawn 行落进串口,新 shell 的开场白跟上来,提示符回来了——机器不会因为一个程序退场就停摆,这就是 init 等在楼上的意义。

北极星的砖,咱们这一卷再对一遍数:编号报的是 fork 57、execve 59、wait4 61、getpid 39、getppid 110,号位照的是 Linux 原样。寄存器的部分,返回值走 rax 的 C ABI,过门只烧 rax、rcx、r11 的承诺没有破。errno 的表,ECHILD、ENOMEM、EIO 都各就各位了。往后装库的那一卷,musl 编出来的静态 ELF,认的就是今天这部装载器。

该收线了。第一幕到这儿算是收束了:机器会引导、会管内存、会调度、会应答中断、有文件、有盘、有名字,再加上这一卷新添的生进程、换程序、收孩子——一件件都是真跑过的,不是名单上的字。当年第一幕走完的地方,咱们看着手里这个内核,做了一个更大的决定——推倒重来一遍。为什么重来,重来时哪些毛病没有再犯、哪些还是犯了,下一卷咱们歇一站,专门地聊这件事。提示符灭了,机器退了场,盘上多了一座 /bin。到了这一回,程序总算是住在了盘上。
