---
title: "11 · 验收:一卷真盘当根"
description: "面板逐字:两行新增、一行改形、三段收据退场,盘报 8192 扇区。52 件测试全部通过,宿主 43 加机内 9,ext2 宿主 40 案真跑 mkfs。kernel.bin 91264 字节。shell 会话逐字走通:真卷的形状、真 inode 的 stat、落笔回读、ACPI 断电。存储线三卷合龙。"
chapter: 16
order: 11
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - storage
  - filesystem
---

# 验收:一卷真盘当根

收卷三样:面板逐字,测试全过,实弹走通。这一卷的面板添了行——上一卷一口气添十行,这一卷添得少,可每一处都动在要紧的地方。新到的几行,咱们逐字端上来:

```text
[kern] irq on, tick 100Hz
[kern] rtc wall clock 1791889222
[kern] keyboard on, type to echo
[kern] boot task (pid 0) parks here, kernel_init (pid 1) takes over
[kern] kernel_init: root mounted, launching shell in ring 3
cinux shell - type 'help'
```

rtc 那行是新增,排在键盘行前头——问芯片那一章的收成,开机就报一次。顺带交代一句:这行读数每趟开机都不一样,咱们两屏引的是同一趟机器的原文。boot task 那行也是新增,开机故事换讲法的开场白。末行改了形:launching shell 前头多了 root mounted,shell 下楼之前,根已经挂好。还有一段看不见的:三段 [task] 演示收据退场,咱们看着面板瘦了三段。ahci 那行也换了个数,8192 sectors——盘从 1 MiB 的空文件换成了 4 MiB 的真卷,2048 变 8192,四倍。

老面板动的数,咱们也对一对。size 从 `9AD00` 走到 `9CE60`,差 8544 字节,按页凑整两页。pmm 从 32302 走到 32300,少的恰好两页,新住户常住的就这两页,一来一回对上了。gs base 两个地址又挪了座,老读者背得出咱们的话:链接挪一挪它就搬家,您机器上跑出别的数才正常。kernel.bin 91264 字节,上一卷 82528,长出的八千多字节,是 ext2 的读写面、缓存、时钟和 ACPI 的行李。四条构建线的读数,咱们散文式报全:普通线,宿主 43 件加机内 9 台,锁座次的开关关着。地址排查的 asan 线,宿主 43 件。数据竞争的 tsan 线,宿主 43 件。末了一条专查未定义行为的 checks 线,宿主 43 件加机内 9 台。后三条线上,`CINUX_LOCKDEP` 全开着,机内那九台,是普通线跑过一遍之后、checks 线带着座次图再跑的那一遍。

测试的家底,这一卷添得厚,咱们把新来的四份点一遍:time 十案,path 十五案,rtc 六案,加上压轴的 ext2 一家四十案。这四十案的地基值得看一眼:它们真请出 mkfs.ext2,在宿主的临时目录里烤一卷 4 MiB 的 ext2,再装进一块 RAM 盘,块脸的 static_assert 当场作保。覆盖面挑几样念:洞文件读回来是零。魔数改坏的卷,门都进不来。二次 mount 拒载。20 KiB 的文件跨进单间。525 KiB 的文件踩进了双间,落点在外层表第二格所指的内层表。还有一卷被手术过的——根目录末一条的名字长度手改到 40,挂上来念清单,被手术的那个名字从清单上消失,前后左右的条目照旧一条条念过去,谁也不多念一个字节。这一桩值得咱们多看两眼,因为读者容易把它读岔。前一节教的四道自验,其实一道都没拦:末一条的 rec_len 本来就把块里剩的地全包圆,头 8 字节加 40 字节的名字合 48 字节,还在 rec_len 的界内,四道全都过了。真正拦下它的,是咱们念清单进门的第一道:

```cpp
if (item.name_len >= kFsNameMax) {
    return true;
}
```

kFsNameMax 的值是 32,连收尾的空字节也算在内。咱们自己添名字的时候,长过它的当场就拒。盘上要是被手术出一条这么长的,念清单的访客就整条跳过了,咱们数座次的时候根本数不到它。手术刀改的是盘上的字节,清单认的是名字的尺寸——40 比 32 长了,这个名字自然就进不来了。

钟是冻住的,1893456000,时间戳案案可对。机内第 9 台,八个案子,每个案子自己组一条链:盘、队列、缓存、ext2,一层不少。盘是 ext2 的拷贝,测试写脏的是拷贝,您手里的卷不脏。

对了,上一卷的 block 测试,这一卷挪了锤,咱们把两头都对一遍。从前那两案,体检的单任务抡在 32 号,多任务那一串从 64 号起。如今卷头住进了 superblock 和位图,锤挪到 8000 和 8040,卷头让给 ext2。两边各有各的道理:ext2 的机内测试用拷贝的卷,写脏不心疼。block 的锤挪去卷尾,superblock 和位图就不会挨锤。

实弹这一样,咱们把 shell 会话的串口原文逐字端上来:

```text
cinux> ls /
./
../
lost+found/
etc/
hello.txt
cinux> cat /hello.txt
hello from the platter
cinux> stat /hello.txt
  inode: 14
   mode: 33188
  links: 1
   size: 23
  mtime: 1791867901
cinux> cd /etc
cinux> pwd
/etc
cinux> cat motd
Cinux says hello from a real disk.
This line lives in an ext2 volume, baked by mkfs at build time.
cinux> cd /
cinux> echo a real note > note.txt
cinux> cat /note.txt
a real note
cinux> ls /
./
../
lost+found/
etc/
hello.txt
note.txt
cinux> poweroff
cutting power through acpi
m
```

这一屏咱们念四遍。头一遍看形状:./、../、lost+found——这三样,内存盘的清单上从来没有过,它们是 mkfs 烤卷那一下就住进去的。ext2 的版式,屏幕上一眼认出。第二遍看 inode:stat 报 14 号,size 23,正是 hello.txt 那一行的长度。mode 33188,换算成八进制是 0100644,读写给人、别人只读的那套。mtime 1791867901,比这一趟开机的钟早了六个钟头不到——烤盘是构建的时候,戳是 mkfs 盖的,盘上的原样,谁也没动过。第三遍看落笔:cd /etc 之后一个 cat motd,相对路径认了。echo 落出 note.txt,再 ls,它排在清单上——盘上写的字,回得来。末一遍看关机:poweroff 一句 cutting power through acpi,模拟器自己退了场。屏末孤零零的那个 m,是断电之后串口上的残响,咱们转写如实,留在原地。

收线。名字那一卷无盘起家,名字和文件住进堆里。上一卷,盘进了门,扇区听得懂人话。这一卷,盘上的字认得了、写得了,真盘当上了根——存储线三卷,咱们在这间屋里看着它合龙。散场之前,两桩旧事摊开来聊几句。头一桩,行李:ustar 的档案还烤在每一个内核里,解析模块还在册、还在考,缺的只是会调它的启动代码。请出去的路子也现成——构建里那几行打包命令删掉,链接输入里把它摘掉,考它的测试改考别的,一个下午的活。只是这活儿不值得夹在谁的卷里顺手带过,它配得上自己的一页。第二桩,stat 的 6 号:挪回 4 号是一行的改动,可号一挪,咱们自己 shell 里敲的 stat 也得跟着换,机器上又没有第二家客人逼着咱们改,这笔记在这儿,等客人上门那天再动。材料都在这一卷里,哪天开工,伸手就够得着。提示符灭了,机器退了场,note.txt 留在盘上——这一回,字落在真盘上。
