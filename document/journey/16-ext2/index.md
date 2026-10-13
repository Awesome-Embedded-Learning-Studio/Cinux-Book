---
title: "16 · 认字:盘上的字怎么读、怎么写,真盘怎么当根"
description: "盘听得懂人话了,盘上还只是字节。这一卷教内核认字:mkfs 烤好的 4 MiB 真卷当教材,卷的版式一层层摊开,读路径把字节念成文件,写路径往盘上落笔,写穿的缓存垫在中间。路径学会了相对,任务长出了 cwd,时间戳问主板上的芯片要,开机的故事换了讲法:PID 0 停岗,PID 1 接班,挂好真卷才放 shell 下楼。收卷时根是一块真 ext2,存储线三卷在此合龙。"
chapter: 16
order: 0
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - storage
  - filesystem
  - ext2
---

# 16 · 认字:盘上的字怎么读、怎么写,真盘怎么当根

上一卷收卷的时候,咱们撂下一句话:盘已经听得懂人话了,下一卷,教它认字。这一卷,就是那个下一卷。构建脚本也不再拿空文件凑数了——truncate 撑出 4 MiB,请出 mkfs.ext2 烤一卷地地道道的 ext2,debugfs 再往里放三样东西:一个 etc 目录,一份两行的开机问候,一行 23 字节的 hello.txt。盘上的字,构建的时候就烤好了。这一卷的戏,是让内核把它们读出来、写进去,末了把这卷真盘扶上根的位子。

活,咱们照例摊开了点名:卷的版式、读、写、换根,四件大头,头一章逐件开讲。随行的家当一串,头一章点名。

还有一件事,咱们说在卷首。这一卷的新面孔不少,可承重的墙全是旧的:块缓存踩着上一卷值班队列的章程盖,写路径攥着进程那一卷的睡眠锁,挂载的门面一件新转发都不用擦,链是现成的。开机的故事也换了讲法:主流程把机器交给头一个真正的任务,自己退去怠速,面板因此添了行,验收那一章咱们逐字对数。

<StationPanel tag="r16_ext2" build="cmake -B build && cmake --build build --target run" />

<ChapterNav variant="sub">
  <ChapterLink num="1" href="01-why-learn-to-read" desc="命令通道立住了,盘上的字节还只是字节。构建期烤一卷 4 MiB 的真 ext2 当教材:mkfs 定版式,debugfs 放种子,hello 在盘上等着。四件活点名:版式、读、写、换根,外加一串随行的家当">为什么是认字</ChapterLink>
  <ChapterLink num="2" href="02-the-volume-layout" desc="头 1024 字节留给引导块,superblock 坐 2 号扇区。卷切成块组,每组自带两张位图和一张 inode 表,教学卷 4 MiB、1 KiB 块、128 个 inode,整卷就是一组。座位表拿 static_assert 压进编译期:superblock 1024 字节、组描述符 32 字节、inode 128 字节,一格不许差">卷摊开来的样子</ChapterLink>
  <ChapterLink num="3" href="03-the-mount-gate" desc="mount 六道检查:魔数 0xEF53、形状(只认 1 KiB 块、不认不兼容特性、inode 尺寸下限加整除)、计数彼此咬合、组数至多 32 封顶 256 MiB、二次挂载拒载、根 inode 必须是目录。组描述符表一口气读进内存常驻,坏卷大声拒,不半懂">认卷之门</ChapterLink>
  <ChapterLink num="4" href="04-reading-into-files" desc="lookup 拿 PathCursor 逐组件下爬,同一对纯函数第三回上场。inode 定位是三行算术,hello.txt 的 14 号现场走一遍。找名字和报清单共用一副骨架,rec_len 四道自验,坏卷不硬读。直接十二格、单间到 267、双间到 64 MiB,三重响亮地拒,洞读回来是零">把字节念成文件</ChapterLink>
  <ChapterLink num="5" href="05-writing-to-the-platter" desc="一把睡眠锁罩住全部写面:找位、置位、改计数、写 superblock,得一口气说完。位图收的是盘上的词,adopt 绑进来就用。目录添名字走两条路,死槽复用、活槽缩缝,塞不下就办不成,绝不给目录添新块。删名字只抹号,字还躺在块里,fsck 的路子留给以后的某天">往盘上落笔</ChapterLink>
  <ChapterLink num="6" href="06-write-through-the-cache" desc="ext2 的活压在同几块上,块缓存 8 槽替队列挡下重复的命令。patch 进槽再整槽下盘,写完这一拍槽和盘是同一份,盘拒收槽标死,宁可不答,不伺候假数。门面手艺第三间:这一卷一件转发都不擦,ext2 直接住进链里,编译期一颗 static_assert 盖章">写穿的缓存</ChapterLink>
  <ChapterLink num="7" href="07-the-relativity-of-names" desc="任务长出 cwd,折路的语义从 shell 下放进内核:FoldOntoPath 折点、弹层,缓冲兼作弹层的栈,shell 自攒的拼路函数整段退休。getcwd 79、chdir 80、reboot 169 同号。stat 坐了 6 号,人家号表上那是 lstat 的座,咱们自己指出来">名字的相对论</ChapterLink>
  <ChapterLink num="8" href="08-asking-the-clock" desc="时间戳要 Unix 秒,钟问 CMOS 芯片要:两只端口一问一答,读两遍全同才采信,BCD、12 小时、世纪寄存器各有各的方言,解码是纯函数,宿主上四种组合都驱过。ext2 不认得芯片,构造时收一只函数指针,盖的是递进来的钟">问芯片几点了</ChapterLink>
  <ChapterLink num="9" href="09-the-new-boot-story" desc="主流程改名 boot task,PID 0 退进怠速,kernel_init PID 1 接班:挂卷、报面板、放 shell。文件世界搬进任务,因为块队列会睡,boot 栈上没人可调度。三段演示任务光荣退役,面板瘦三段。关机走 ACPI:EBDA 扫到表根,表里请出 FADT,PM1a 一笔 16 位写出去,断电前串口等空">开机的新故事</ChapterLink>
  <ChapterLink num="10" href="10-the-shell-grows-up" desc="stat 五行、cd、pwd、poweroff 四件进门,菜单一口气排到十三样。名字那一卷代码里立起的单词矮墙,这一卷撤了。poweroff 办不成撂一句俏皮话。行李还链在镜像里,会调它的启动代码一个不剩,如实一句">shell 的成年礼</ChapterLink>
  <ChapterLink num="11" href="11-acceptance" desc="面板逐字:两行新增、一行改形、三段收据退场,盘报 8192 扇区。52 件测试全部通过,宿主 43 加机内 9,ext2 宿主 40 案真跑 mkfs。kernel.bin 91264 字节。shell 会话逐字走通:真卷的形状、真 inode 的 stat、落笔回读、ACPI 断电。存储线三卷合龙">验收:一卷真盘当根</ChapterLink>
</ChapterNav>

合卷的时候,机器换的是根基:根不再是一段烤在镜像里的行李,而是一卷写得住字的真盘。三卷各自怎么走到这一步,验收那一章咱们收拢,这里不抢戏。
