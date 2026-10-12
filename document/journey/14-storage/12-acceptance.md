---
title: "12 · 验收:面板没添一行"
description: "收卷三样:面板十二行一行没多,只对 size 与 pmm 两个数——内核长的那七页,正是 pmm 少的那七页;四十三件测试全部通过,kernel.bin 70208 字节;shell 会话逐字走通,initrd 两份文件开机即在,echo 重定向落出 hi.txt,新名字回回排最前,头插的形状屏幕上见真章。名字与文件在内存里站住,真正的盘排在后面。"
chapter: 14
order: 12
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - filesystem
  - shell
---

# 验收:面板没添一行

收卷的证,还是那三样:面板逐字、测试全过、实弹走通。这一卷有个跟哪一卷都不一样的脾气:开机面板一行都没添。卷首咱们预告过,这一卷看得见的变化全在 shell 的会话里——所以头一样咱们念得短,戏留给第三样。

面板十二行,咱们原样端上来:

```text
[kern] 64-bit C++ world alive
[kern] gdt+idt self-owned, sse on
[kern] tss loaded tr=18 gs base FFFFFFFF80292600 efer 501
[kern] kernel gs base FFFFFFFF80292600
[kern] bootinfo: 7 e820 entries, fb 1024*768*32
[kern] screen console 128 cols 48 rows
[kern] kernel at 200000 size 976C0 entry 200040
[kern] pmm: 32309 pages free (probe 113000 ok)
[kern] vmm: address space up, magic 114514
[kern] heap: new/delete live (probe 114514)
[kern] irq on, tick 100Hz
[kern] keyboard on, type to echo
```

跟上一卷收工的面板比,行的数目、行的次序、行的措辞,一样没动。动的只有两个数。头一个是 size,976C0,上一卷是 90A20——内核长大了。长了多少,咱们换算给您看:两个数相减,差 27808 字节,不满七页。可镜像占地方按页算,两头向上凑整,内核恰好多占了七页。第二个数是 pmm 的空闲,32309,上一卷是 32316,少的恰好也是七页。多占的那七页,正是物理内存少掉的那七页:文件世界、它的锁、它的册子,连同肚子里那座小档案馆,住的就是这七页。一来一回,数对上了。至于 gs base 两个地址,老读者背得出咱们的话:链接挪一挪它就搬家,您机器上跑出别的数才正常。bootinfo 行里 e820 的条数是模拟器当趟给的,不参与对数。

第二样验的是测试,咱们把数目报全,口径也报全。测试台上一共四十三件全部通过:宿主侧三十六件,机内七台。件数说的是测试的名目,新写的九份测试,里头的用例添了七十一条:ramfs 二十三条,门面九条,文件册九条,档案格式八条,槽表六条,锁座次六条,定深栈五条,引用计数三条,有向图两条——九份加拢,恰好七十一条。文件册那章立的收尸案,在机内的 sched 台上跑。这四十三件,是普通构建线上的读数。仓库自带的验收脚本把构建线一共铺了四条,后三条换着加检查的开关:地址排查的 asan 线,宿主三十六件。数据竞争的 tsan 线,宿主三十六件。末了一条专查未定义行为的 checks 线,宿主三十六件加机内七台。后三条线上,锁座次的开关 `CINUX_LOCKDEP` 全开着——机内那七台测试内核,是带着座次图跑绿的。产物一并交底:内核本体 kernel.bin 是 70208 字节,上一卷是 43456,长出来的两万六千多字节,大半是行李和接行李的新家当。整张启动镜像,还是 1 MiB。

第三样,实弹。面板滚完三段调度器的旧收据,常客下楼,咱们把键一组一组递进去,串口原文逐字贴在下面:

```text
cinux shell - type 'help'
cinux> ls /
docs/
readme.txt
cinux> cat readme.txt
welcome to cinux - this file was baked into the kernel image
cinux> cat docs/note.txt
station 14: the kernel knows files before any disk exists
cinux> echo hello ramfs > hi.txt
cinux> ls /
hi.txt
docs/
readme.txt
cinux> cat hi.txt
hello ramfs
cinux> mkdir notes
cinux> ls /
notes/
hi.txt
docs/
readme.txt
cinux> rm hi.txt
cinux> ls /
notes/
docs/
readme.txt
cinux> 
```

这一屏咱们念三遍。头一遍看来历:开机第一句 ls,报出来的就是 docs/ 与 readme.txt——机器的盘上什么都没有,这两样全是镜像里烤着来的行李,cat 出来的两行,跟栽树那一章交底的一字不差。第二遍看次序:echo 重定向落出 hi.txt 之后,清单头一行是它。mkdir 的 notes 之后,头一行换成了 notes/。树是头插的,新来的排最前——头一棵树那一章立下的形状,屏幕上一验就着。巧合解释不了它,这就是链表长成的样子。第三遍看退场:rm 掉 hi.txt,再 ls,清单干干净净,链上摘掉一个名字,册面不留影子。整段会话从头到尾,没有一块盘参与过——机器上压根就没有盘。

末了把卷首的话收拢。上一卷立下的编号、寄存器、负 errno 三块砖,这一卷一块没改:新业务的五个号全照 Linux 落座,负 errno 报满十种,寄存器的座位原地不动。将来装库的那一卷,人家编译好的二进制敲的还是这套门,认的还是这套号。名字和文件,今天在内存里站住了。真正的盘排在后面——等它进门,往这座门面上再挂一个后端,客人们念的,还是咱们今天立的路名。
