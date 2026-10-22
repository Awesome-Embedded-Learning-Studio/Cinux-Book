---
title: "23 · 开门迎客:musl、busybox 与行编辑的终端"
description: "同一副身家那一卷收卷时名单只剩最后一件:装库,跑别人编好的程序。这一卷把门开大:musl 静态工具链进门,busybox 顶着 ash 上岗,十九个号位一次铺开,stat 的座次与返回布局两案并破,管道一家四号立起,目录的念法改成打包,行编辑搬进内核,ppoll 开张,细一档的钟从 ACPI 表里请出来,四句头里答着的假话全部换成真话。手搓的 shell 在这里交棒,北极星指的那一天到了。"
chapter: 23
order: 0
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - musl
  - busybox
  - pipe
  - tty
---

# 23 · 开门迎客:musl、busybox 与行编辑的终端

同一副身家那一卷收卷的时候,名单上只剩了最后一件:装的是库,跑的是别人编好的程序。咱们当时还把客人的样子描过一笔:自带的是一整箱家什。这一卷就是来开门的。等门开完了,机器上跑的就不再只是咱们自己编的那几份小程序了,咱们把别人编好的、一具身子里装着四百多个命令的大程序,也请进了门,坐上了 shell 的座。

咱们动工之前照老章程盘家底。手里有的,是四卷攒下的供给线:生与收那一卷的 fork、exec 与收尸,懒到家那一卷的按需给页,听得懂喊声那一卷的信号全套,同一副身家那一卷的线程与描述层,后来咱们拿真客人逐号对过,单子确实是全过的。缺的咱们也一片一片地数:号位表上 musl 要敲的号,还有一整片是空的。stat 的座次,自家对着自家错了多年,真 ABI 的客人一敲就破。管道是没有的,一条命令的输出接不进下一条命令。fd0 交出去的是裸键,行编辑是没有的。ioctl 与 ppoll 也是客人问话的门,咱们也还没有开。

这一卷的活,咱们摊开报数。头一程是工具链与四层的旧病:客人还没进门的当口,自家的装载器就被它的段表逼出了四个潜伏的真病。第二程说的是落地的三刀:诊断的打表救的是它自己,一笔寄存器上的笔误,一道放宽了的补页门规,换来的是 ash 咽下的第一句命令。第三程是号位的大迁移:十九个新号一次铺了出来,stat 的座次考了古,入口的画像补成了全参,三处并发的窗口也关上了。第四程讲的是两案与一管:凌晨销掉的两桩悬案,管道一家四号立了起来,目录的念法改成了打包,说谎的 mode 被咱们当面拆穿,大盘与默认的路名落了定,细一档的钟也从 ACPI 表里请了出来。末一程说的是终端与真话:行编辑被咱们搬进了内核,咱们亲手一玩补齐了三件,四句头里答着的假话全部换成了真话。收尾照旧走的是验收。

当年的旧话咱们也照例交代。这一段在工程线上是全教程最长的一站:busybox 进了门,手搓的 shell 交了棒,当年用的是 1.36 的版本,活是连着几天发的。第一遍的读者就是被这样的密度喷倒的,所以幕间为它立过家法。这一遍咱们把它拆成三卷来消化:本卷管的是把客人跑起来,后面一卷管的是假文件与真终端,再后面一卷管的是 ELF 动态。生与收那一卷的话还挂在明处:动态链接的学问,是留给装库这卷再展开的。装库的这卷今天到了,咱们把话分清:库咱们只请静态的,动态链接的学问自有单独的一卷,这一卷一个字也是不碰它的。

<StationPanel tag="r23_run_progs" build="cmake -B build && cmake --build build --target run" />

<ChapterNav variant="sub">
  <ChapterLink num="1" href="01-why-open-the-door" desc="北极星指着的那一天:下楼开张那一卷立柜台时说定,装库跑别人的程序,人家认的只是那套约定。四卷供给线的单子摆开,手搓的 shell 交棒交的是什么,静态与动态的分界在卷首说定">为什么开门迎客</ChapterLink>
  <ChapterLink num="2" href="02-a-toolchain-and-four-old-debts" desc="宿主上自编 musl 1.2.5,busybox 1.38 静态编出来,一具身子四百零七个命令,段四的虚地址不页对齐。客人顶进 /bin/sh 的座,四层旧病连着炸出来:exec 的栈窗、init 的第二现场、装载器的头半页、入口的画像">一套工具链,四层旧病</ChapterLink>
  <ChapterLink num="3" href="03-a-diagnosis-that-ate-itself" desc="死面打转,误诊成堆区没人补页,重验才知诊断打表自己吞了第一因。三刀:打表的第一层漏了物理转虚地址,getcwd 把 size 写去了 rsi,补页的门规放宽成只看承诺。ash 活着吃下第一句 echo">打表救的是它自己</ChapterLink>
  <ChapterLink num="4" href="04-nineteen-numbers-one-wrong-seat" desc="strace 四组抄单子,时钟在宿主上走 vDSO 全然不见,咱们没有,只能真做。十九个号一次铺开,表界挪到三百一十九。ash 报 Function not implemented,探针打的是四号:stat 六个卷都坐在 lstat 的座上,busybox 头一个按真 ABI 敲的就是四号">十九个号位,一把坐错的座</ChapterLink>
  <ChapterLink num="5" href="05-three-versions-three-windows" desc="execve 的画像补成全参:字符串页底向上,指针从 rsp 正向排,三版才定形,页装不下答 E2big。完成件的窗口被崩溃循环逼出原形,置 blocked 挪进锁内。I/O error 的案卷请外面的眼睛看了一遍,回来三刀:泊车竞态的第二层、块缓存的悬空指针、整读的 Result 化">画像三版,窗口三处</ChapterLink>
  <ChapterLink num="6" href="06-two-cases-closed-at-dawn" desc="ENOENT 是乌龙:复现配置引用了盘上不存在的路径,如实记档,二十连发全过,复跑脚本入仓。ash 的 segv 根因在信号帧:句柄返回后栈顶已是上下文本体,sigreturn 多解引用一层,红区没留,指针槽被句柄的压栈盖掉,动作记录改成 musl 认的词序">凌晨销掉的两案</ChapterLink>
  <ChapterLink num="7" href="07-one-pipe-four-numbers" desc="pipe、pipe2、dup、dup2 一次立齐。管道穿文件后端的外衣,read 与 write 的 handler 一字不改就吃管道 fd,写端计数管告别,末位写者唤醒读者答零。dup 认领同一份描述,槽表添 place 新词。ash 的竖线全链走通,盘的余量敲响了警钟">一根管子,四个号</ChapterLink>
  <ChapterLink num="8" href="08-packed-records-and-a-lying-mode" desc="getdents 从一次一条改成打包式,字段按偏移落笔,自家 shell 的 ls 改解包。ioctl 对 fd0 到 fd2 答零 termios。裸命令稳定报 Permission denied,排查收进一个矛盾:13 出在用户态,execve 却没进过内核。定案:stat 的教学四十字节对不上 musl 要的一百四十四字节,假 mode 恰好没有执行位">打包的记录与说谎的 mode</ChapterLink>
  <ChapterLink num="9" href="09-a-bigger-disk-a-finer-clock" desc="盘从四兆扩到十六兆,inode 添到五百一十二,双名住下的是实弹另烤的一次性副本。envp 递空时 exec 铺一条默认 PATH。HPET 的两个旧解释被实验推翻后如实收回,真因是 ACPI 表的字段各晚四字节,period 读回的是 BIOS 中断向量表的内容。三道修复,商余分解防中间积溢出,隔几十秒两发 date 对上真时间">大一号的盘,细一档的钟</ChapterLink>
  <ChapterLink num="10" href="10-the-discipline-runs-in-read" desc="终端的真身住进 kernel/driver/tty:行规程在 read 里跑,不在中断里。读者逐键取,feed 管编辑,整行压进行池,read_line 阻塞到有行。自家 shell 撤掉用户态回显防双重。测试纠偏两笔如实记,退格的疑云病在工具的键名表">行规程在 read 里跑</ChapterLink>
  <ChapterLink num="11" href="11-hands-on-the-keyboard" desc="亲手坐到键盘前,玩出两断点一噪声:串口只发不收,read_line 改双源。musl 的 isatty 真探的是 TIOCGWINSZ,旧说当卷更正。每键一行 syscall 271 unserved,ppoll 做全量不做桩:就绪按描述符答,半行也算,掩码收下不验记给下一卷">亲手一玩,三件补齐</ChapterLink>
  <ChapterLink num="12" href="12-four-lies-retire" desc="铺号那晚头一批答着的四句假话全部换真:fcntl 的状态面与座位面分家,关 exec 的位住进文件表,prctl 写读真名,prlimit64 立十六行真表,getrandom 接上 RDRAND。回头扫树把裸数领名字,base 的家底两例进了文件主题">四句假话,换成真话</ChapterLink>
  <ChapterLink num="13" href="13-acceptance" desc="开机面板添一行 hpet。宿主五十一件长到五十三件,机内十三台长到十五台,整条 ctest 六十八件,四条配置线全过。busybox 实弹链逐场念,复跑脚本二十连发,复阅收卷。欠的挂明处,客人指名的下一批东西交给下一卷">验收:客人住下了</ChapterLink>
</ChapterNav>

合卷的时候咱们回头看,机器的身份换了:从今往后的它,是一台跑得了别人程序的机器。北极星的三条约定:编号、寄存器、负 errno,挂了十个卷,到这一卷才算全数兑了现:人家的表怎么排号,咱们就怎么挂,人家的结构长什么样,咱们就怎么给,连动作记录里词的次序,咱们都照人家的摆。手搓的 shell 交了棒:造盘的脚本指谁,盘上 /bin/sh 的座就是谁的,老 shell 还是在岗的,可 ash 是上得去的。欠的也照旧挂在明处:写往空管道的那对事件,词是备着的,挂点是没有接的。键盘的回显与行编辑叠出来的双重回显。ppoll 收下未验的掩码。termios 的开关面。入口画像的机内案,目录缓冲跨重拉的挂案,HPET 的机型记档,O_* 一整类的收编。还有前几卷老欠的几笔。客人住下了,它伸手指名的下一批东西,假文件与真终端的名目,是下一卷的活。
