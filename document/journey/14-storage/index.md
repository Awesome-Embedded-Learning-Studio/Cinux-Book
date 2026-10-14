---
title: "14 · 无盘起家:名字、内存里的盘与随身的行李"
description: "上一卷收在盘上一无所有,这一卷把次序倒过来开工:盘还没有,文件和名字就住进了内核的堆里。路名怎么念、名字多长算长,头一棵树把名字挂成形状,ramfs 把字节种进堆里,VFS 门面把后端藏进挂载表,ustar 行李烤进镜像开机栽树。柜台添五件业务号码照 Linux,文件册每任务一本,锁立起座次图。面板一行不添,变化全在 shell 会话里,收卷时名字与文件在内存里站住,真正的盘排在后面。"
chapter: 14
order: 0
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - filesystem
  - ramfs
  - initrd
---

# 14 · 无盘起家:名字、内存里的盘与随身的行李

上一卷收卷的时候咱们交过底:echo 打出来的字是您敲的,help 报的菜名是编进镜像的,shell 自己睡在内核怀里,整台机器的盘上什么都没有。程序想从盘上被请进来,得等有了盘,有了文件,有了名字。这一卷开工的正是这摊差事——咱们把它倒了过来:盘还是连影子都没有,文件和名字就住了进来,住在内核的堆里,开机的时候还随身带着两份编译期烤好的行李。当年这一段是从盘的实体动工的,盘的驱动连着盘上的文件系统一大段修完。这一遍把次序倒过来,理由摆在地上:名字的世界立稳了,真盘哪天进门,就是往门面后面再挂一个后端的事。

这一卷的路,咱们分三段走。头一段立名字的世界:一对纯函数管路名怎么念,三行配置管名字多长算长。名字挂在头一棵树上,结构归树、货归后端,谁也不必重写爬树的路子。ramfs 把字节种进堆里,引用计数替开着的文件撑命。一座门面把后端藏进挂载表,行李的格式,咱们捡了磁带时代的老 ustar。第二段总装与开张:构建期把行李烤进镜像,开机逐条目栽进树里。柜台一口气添五件业务,每个任务领一本文件册,目录也学会了被一条一条念,shell 添了五件新手艺,hello 光荣退役。第三段收好家底:给全内核的自旋锁立一张座次图,死锁的种子当场揪,末了把面板、测试和整段会话摆上桌验收。

还有两句话,咱们说在卷首。头一句:这一卷的开机面板一行不添,老十二行还是老十二行,变的只有里头两个数——这一卷看得见的变化,全在 shell 的会话里,验收那一章逐字见。第二句:上一卷立下的三块砖,编号、寄存器、负 errno,这一卷兑现成柜台上的五件新业务,一格没改。北极星还挂在那儿,将来装库那一卷里,人家编译好的二进制认的就是这套约定。

<StationPanel tag="r14_vfs_ramfs_initrd" build="cmake -B build && cmake --build build --target run" />

<ChapterNav variant="sub">
  <ChapterLink num="1" href="01-why-names-before-disks" desc="机器无盘,程序想进门得有文件和名字。决定:头一个文件系统长在内核的堆里。四件活点名:名字的规范、头一棵树、一座大门面、一份随身的行李。开机面板一行不添,变化的舞台全在 shell 会话里">名字排在盘前</ChapterLink>
  <ChapterLink num="2" href="02-how-paths-read" desc="PathCursor 逐组件发路名,多重斜杠照收,全斜杠发空;SplitLeaf 在末道斜杠处分父路与叶。名字 32、路径 96、挂载 8,三常数一份配置,全楼层共用一张尺寸表。路径以视图旅行,一个字节不抄">路名怎么念,名字多长算长</ChapterLink>
  <ChapterLink num="3" href="03-the-first-tree" desc="CRTP 把结构半边长进每个后端的节点:名字、类型、号、兄弟链归树,货归派生。孩子头插,最新的排最前,ls 的输出次序从这里来。named 三重比对防前缀搅局,disown 摘边不销毁,WalkPath 逐组件下爬,半道踩着文件回 kNotADirectory">第一棵树</ChapterLink>
  <ChapterLink num="4" href="04-ramfs-in-heap-bytes" desc="RamNode 是树的半边加 unique_ptr 字节缓冲,RefCount 管生死:孩子边与开着的描述符各占一份,名字摘了照读,最后一放手就地消失。一把自旋锁守全部操作,扩容按页粒度,一 MB 一字节一写只搬二百五十六次家,空洞补零不漏陈年老字节">长在内存里的盘</ChapterLink>
  <ChapterLink num="5" href="05-the-mount-facade" desc="九个方法一份 concept 编译期把关,后端就是个普通类;挂载那一下 EraseFs 生成九个无捕获转发,零手写样板。挂载表固定八槽,同前缀 kAlreadyExists。resolve 只认组件边界,/tmp 罩不住 /tmpx;同路多罩,深的赢;余路不带前导斜杠">一座大门面</ChapterLink>
  <ChapterLink num="6" href="06-the-luggage-format" desc="磁带时代的老格式 ustar:512 字节的头部块,名字在 0 号位宽 100,大小在 124 号位装八进制 ASCII,61 个字节躺成 00000000075;类型旗 '0' 文件 '5' 目录,魔数坐 257 号位,全零块收尾。校验和域没验,如实交底;条目零拷贝,视图直指档案内存">行李箱的格式</ChapterLink>
  <ChapterLink num="7" href="07-planting-at-boot" desc="构建期三步:tar 按 ustar 打包两份资产,objcopy 改节名 .rodata.initrd、改符号 g_initrd_start 与 g_initrd_end,链接器接进每个内核。FileWorld 又一位住进单例编制:new ramfs、挂根、逐条目栽树,步步 Check,坏档案按构建 bug 停机。kernel.cpp 真正插进启动流程的只有一行">开机栽树</ChapterLink>
  <ChapterLink num="8" href="08-the-counter-renovation" desc="柜台添五件业务:open 2、close 3、mkdir 83、unlink 87、getdents 217,号照 Linux,编号的砖一格没改。一号一间,分发表 218 格,空槽 -ENOSYS;错误名册从六格添满十格,ErrnoFrom 一处翻成负 errno。CopyUserPath 查边界、内核栈 96 字节落地,open 查无且带 0x40 才创造">柜台翻新</ChapterLink>
  <ChapterLink num="9" href="09-a-table-per-task" desc="文件册每任务一本,按值住 Task。0、1、2 是固定座,柜台那层就接走;发号从 3 起,至多五份文件同开。一格记后端、节点与 offset——文件按字节,目录按条目。alloc 在关中断下占引用,发最低空号;任务退场析构链挨格放还,机内测试 retain 与 release 各对上两笔">一张跟着任务走的文件册</ChapterLink>
  <ChapterLink num="10" href="10-reading-directories" desc="getdents 一次念一条,offset 当条目序号,念完加一;号与语义照 Linux,打包缓冲的形状是教学的。shell 添 ls、cat、touch、mkdir、rm 五件手艺,echo 学会重定向;user API 添五个内联包装,两个世界读同一张表;hello 退役,常客的岗位一位就够">目录的念法与 shell 的新手艺</ChapterLink>
  <ChapterLink num="11" href="11-the-lock-order-ledger" desc="锁的座次画成有向图:顶点是活锁的地址,边是持着谁又去拿谁,连边当场查环,响着死不悄悄漏。释放只许弹栈顶,retire 销户赶在存储回收之前。三件新容器——槽表、定深栈、有向图——一处交底;边界如实:只看自旋锁,默认关着,验收脚本 acceptance.sh 四条构建线全跑">锁的座次图</ChapterLink>
  <ChapterLink num="12" href="12-acceptance" desc="面板十二行一行没多,只对两个数:size 976C0、pmm 32309——内核多占的七页正是物理内存少的七页。四十三件测试全部通过,kernel.bin 70208 字节。shell 会话逐字走通:行李开机即在,新名字回回排最前,rm 干净退场,全程无盘">验收:面板没添一行</ChapterLink>
</ChapterNav>

合卷的时候,机器还是原样,面板一行没多,可您到 shell 的会话里看一眼,已经换了人间:ls 念得出目录,echo 落得出文件,rm 走得干干净净,名字的次序回回是新的在前。这些名堂没有一样踩在盘上——文件和名字住在堆里,行李烤在镜像里,底细记在每一份文件册里。真正的盘排在后面,它进门的那一天,要做的不过是往这座门面后面再挂一个后端,客人们念的,还是咱们今天立的路名。
