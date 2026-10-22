---
title: "17 · 生与收:fork、execve 与收尸"
description: "盘上有字,机器却只会跑一个烤在镜像里的 shell,装载器贴着一页的天花板。这一卷教机器生养进程:家族立起 pid 与僵尸,写时复制带着引用计数一次立对,fork 合成一帧从返回尾进场,execve 改旧帧换一个新世界,wait 收下孩子的退出码。收卷时 shell 住进盘上的 /bin,init 等在楼上,孩子退场就重拉,第一幕在此收官。"
chapter: 17
order: 0
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - process
  - fork
  - exec
---

# 17 · 生与收:fork、execve 与收尸

认字那一卷收了卷,根已经是写得住字的真盘。可您数数机器会跑的程序,只有一个——shell 还烤在内核的镜像里,装载器只会把一页的字节搬进 0x400000,实测的 3657 字节,贴着 4096 的顶。盘上是有字的,咱们认得出也读得回,却没有哪份程序是能从盘上被请进来的。这一卷咱们把这句话作废:fork 造了进程,execve 换了程序,wait 收了孩子,盘上开出了 /bin,shell 自己也搬了家。

活分了两拍走。头一拍咱们让 fork 在测试里成立:家族的词汇、死者之家、写时复制的引擎、fork 的本体,全在测试世界立起来跑绿了,ring3 端的一点不知情。第二拍咱们通电:shell 添一个 run 命令,三桩案子连着上门——被覆盖的返回值、一个来历不明的五位数、两条链挤一枚节点——咱们挨个破掉之后,run /bin/hello 的全链真跑,孩子交出了 42,父亲报数收了尸。

路上还有几样真的东西,值得咱们在卷首报数。CR0 的 WP 位从加电起就是零,只读页对内核是从来不管束的,这一卷把它拉了起来,写时复制才有了铃可听。一帧两映射的测试重写教咱们:构造性测试的绿,要禁得起探针的证伪。Floor 的页号语义,同一天教了咱们两遍。当年的 fork 在栈上炸过三幕,后日谈里从凑合修一路讲到了 ret_from_fork,咱们这一遍靠着后见之明直取终态。

<StationPanel tag="r17_fork" build="cmake -B build && cmake --build build --target run" />

<ChapterNav variant="sub">
  <ChapterLink num="1" href="01-why-the-machine-cannot-run-programs" desc="盘上有字,机器只会跑一个烤在镜像里的 shell:装载器一页硬天花板,3657 字节贴着 4096 的顶。要跑程序得有整套手续:fork、execve、wait、僵尸。活分两拍:头一拍测试里成立,第二拍通电真跑">机器还跑不成程序</ChapterLink>
  <ChapterLink num="2" href="02-family-groundwork" desc="Task 立 pid、parent、exit_code,kZombie 入列,exit 分流:有父变僵尸定向唤醒,无父直通回收。wait_child 阻塞到指定子退场,死者之家安置孙辈。祖母测试拿丢孩子 bug 是问,红路探针证明测试有牙,基线取样三犯教咱们放对尺子">家族的地基</ChapterLink>
  <ChapterLink num="3" href="03-what-fork-copies" desc="fork 的知识主菜:映射、现场、fd、cwd 逐件过堂。内核高半区天然共享,分界立在根表中间 256 格。急拷与写时复制摆在桌上,咱们选后者,引用计数一次立对,当年没计数的那版留在纸面当反面教材">复制什么,不复制什么</ChapterLink>
  <ChapterLink num="4" href="04-the-cow-engine" desc="kCow 坐 bit 9 的软件位,MarkPageShared 降级加计数。CR0.WP 全程为零的真发现:ring0 写穿只读页不响铃,KernelStart 从此拉起 WP。缺页裁决者坐进异常报告员的单座,有伴复制、末位复权。一帧两映射的测试重写,构造性测试的绿要能被证伪">写时复制的引擎</ChapterLink>
  <ChapterLink num="5" href="05-fork-itself" desc="被 96 字节帧遗漏的 r12-r15 走 GS 侧四格银行,BuildForkChild 持守卫克隆世界、拷一帧进新栈,ForkChildEntry 镜像返回尾、rax 清零。帧计数升格进 PMM,每帧一字节。克隆测试恒漏一页,自查三回未中,回头再读一眼命中:frame_index 把 Floor 的返回值又除了一次页大小">fork 本体</ChapterLink>
  <ChapterLink num="6" href="06-the-elf-loader" desc="kernel/loader 目录出生:elf_layout 纯词汇件校验头,装载件逐段逐页,整页清零加文件字节前铺,bss 天然零出身。页内偏移是主角:超一页的段第二页起全零,当年炸过,这遍换了衣裳又来。手搓字节流三错改程序化造像,UBSan 线逮住未对齐读">装载器认 ELF</ChapterLink>
  <ChapterLink num="7" href="07-execve" desc="fork 造新帧、exec 改旧帧,一对手法共用一条返回尾:改写当前帧的 user_rsp、rcx、r11,借正常的 syscall 返回进新程序。次序是挂新、切表、后拆旧。栈顶撞名异值顺手收掉,argv 的留白照实说">execve 换世界</ChapterLink>
  <ChapterLink num="8" href="08-three-cases" desc="通电头一晚三案齐发:gs:16 中转被抢占覆盖,父进程误判自己是孩子,修法是 rax 直通。换了世界不切表,神秘数字 5246548 当场破译。SelfNode 双链踩踏:shell 同挂两条链,一枚节点一个 next,ktest 全过实弹照炸,一链一节点的家规立起">三案连破</ChapterLink>
  <ChapterLink num="9" href="09-the-loop-closes" desc="Own 家族入库,出界即还,AddressSpace 析构自拆,fork、execve、init 全线收伞。镜像里的 .user 行李整段拆除,shell 改出 ELF 搬上盘,kernel_init 从 /bin/sh 起用户世,wait 到退场就重拉,上一卷借来的称呼全部还清">收官:伞、盘上的 shell、respawn</ChapterLink>
  <ChapterLink num="10" href="10-afterword-the-frame-pointer" desc="fork 的栈语义当年走了三部:头一幕 -O2 下 RBP 不可信,凑合修。第二幕 376 字节的窗口,16 KiB 栈写穿,页故障同栈再故障,三重故障。gcc 两猜皆错,末了按 Linux ret_from_fork 范式根治——正是这一遍直取的终态">后日谈:帧指针三部曲</ChapterLink>
  <ChapterLink num="11" href="11-acceptance" desc="面板安静,只改一行:shell 从卷上来,实跑读数 size 49DF00、pmm 31275,kernel.bin 95488 字节。测试 52 走到 56,宿主 44 加机内 12,新套件四份,test_sched 添十案。实弹会话逐字:run /bin/hello,问候贴着 [3] exited with 42,exit 之后 init 重拉 shell。三块砖对数,第一幕收束">验收:会生会收的机器</ChapterLink>
</ChapterNav>

合卷的时候,机器换的是活法:程序不再生在镜像里,它成了一份文件,从盘上被请了进来,跑完了交一个码,交出的码有人收,退了的场有人重新拉起。生、换、等、收这四件事合起来叫进程的生命周期,第一幕的零件到这儿配齐。下一卷咱们歇一站,回头看看当年为什么要推倒重来一遍,再往后的路,要带着这套家底去按需分页、去装库、去跑别人的程序了。
