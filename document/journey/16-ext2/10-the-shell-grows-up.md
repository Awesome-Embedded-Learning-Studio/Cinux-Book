---
title: "10 · shell 的成年礼"
description: "stat 五行、cd、pwd、poweroff 四件进门,菜单一口气排到十三样。名字那一卷代码里立起的单词矮墙,这一卷撤了。poweroff 办不成撂一句俏皮话。行李还链在镜像里,会调它的启动代码一个不剩,如实一句。"
chapter: 16
order: 10
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - storage
  - shell
---

# shell 的成年礼

四件新手艺进门:stat、cd、pwd、poweroff,菜单从九样一口气排到十三样。cd 和 pwd 是一对,poweroff 是头一件办成了就不回头的业务,咱们挨个过。

stat 的五行,验收那一章您会在屏幕上见到:inode、mode、links、size、mtime,盘上的 inode 原样报上来,一个字段不加工。cd 走 80 号:折路,验目录,cwd 搬家。pwd 走 79 号,把 cwd 抄给客人。poweroff 走 169 号,上一节铺好的 ACPI 路。固件要是没把表递出来,柜台回 `ENOSYS`,shell 报一声 errno,末了撂一句俏皮话:`machine still standing; firmware ignored the sleep word`——机器还站着,固件没理会那个睡觉的词。

老业务身上也有几处顺手的小修整,咱们点个名。命令的参数,这一卷起再不经过任何拼装,shell 拿到的词原样递进柜台,折不折、怎么折,全归内核新开的折路走廊。报错的路子还是上一卷的 report_errno,负数翻成 errno 的那一声:cd 折出一个查无此人的路,屏幕上就是 cd failed: errno 2。yield、exit、help 三位老人,岗位没动。

还有一桩旧事,当时没点破,咱们在这一卷顺手办了。矮墙是名字那一卷的代码里立起来的:shell 挑命令的地方横着一道坎,一行里不满两个词,一律哑火,连开场白里请人敲的 help 都敲不应。矮墙在这一卷撤了:一个词,办一个词的事。名字那一卷的演示屏为了躲它,命令全凑成了双词——如今不消了。

末了一桩,咱们得跟您交底,交的是行李的下落。构建的流水线还在把 ustar 档案烤进每一个内核,链接输入还带着它,镜像里那块地还占着,可当年会读它的那段启动代码,一行不剩。名字那一卷教咱们认树的两份文件,如今是挂在镜像里的死重——它还链在那儿,解析它的模块还在册,肯调它的启动代码一个不剩。留不留,什么时候请出去,卷尾咱们摊开来聊。

手艺到这儿齐了,咱们上秤——下一节,验收。
