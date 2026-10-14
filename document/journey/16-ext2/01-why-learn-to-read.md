---
title: "01 · 为什么是认字"
description: "命令通道立住了,盘上的字节还只是字节。构建期烤一卷 4 MiB 的真 ext2 当教材:mkfs 定版式,debugfs 放种子,hello 在盘上等着。四件活点名:版式、读、写、换根,外加一串随行的家当。"
chapter: 16
order: 1
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

# 为什么是认字

上一卷收卷的时候,盘认下来了:名字、容量、脾气,内核心里全有数,读写扇区的命令通道也跑通了。可您低头看盘上,还是空的——不对,这一卷连空都算不上了,构建脚本这回给机器配的,是一卷写满字的真盘。字是哪来的?构建的时候烤进去的。脚本不长,咱们整个端出来:

```bash
#!/usr/bin/env bash
# Bakes the teaching ext2 volume: 4 MiB, 1 KiB blocks, no features,
# 128 inodes, two seed files from scripts/ext2_seed/.
set -euo pipefail

image=$1
seed_dir="$(cd "$(dirname "$0")" && pwd)/ext2_seed"

truncate -s 4M "$image"
mkfs.ext2 -q -F -b 1024 -O none -N 128 "$image"
debugfs -w -R 'mkdir /etc' "$image" >/dev/null 2>&1
debugfs -w -R "write ${seed_dir}/motd.txt /etc/motd" "$image" >/dev/null 2>&1
debugfs -w -R "write ${seed_dir}/hello.txt /hello.txt" "$image" >/dev/null 2>&1
```

truncate 撑出一块 4 MiB 的空文件,mkfs.ext2 把它变成一卷 ext2。三个参数都值得念一遍:`-b 1024` 说块的尺寸定成 1024 字节,`-O none` 说新式的特性一样都别加,给咱们一张老模样的卷,`-N 128` 说 inode 的定额是 128 个。烤好的卷也不是白纸:ext2 的定例,卷一出生就有个 lost+found 躺在根上。种子是 debugfs 放的,它是 ext2 家自带的小手术刀,不用挂载就能往卷里放东西——一个 etc 目录,一份两行的 motd,一行 hello.txt,23 个字节,内容您在验收那一章会亲眼见到。

有朋友可能要问了:文件系统咱们不是搓过一座吗?搓过的,ramfs,长在堆里。可堆里的格式是咱们自己定的,自己定自己读,顺是顺,顺得学不到东西。盘上这一卷要认的是天下通用的字:mkfs 是人家三十多年的手艺,版式的每一格都有来头,谁写的卷都得照同一张图纸来。咱们拿现成的卷当教材,内核学会了读它,将来的盘、将来的镜像,只要还是 ext2,就都是熟客。

这一卷的活,四件。头一件,卷的版式:superblock、块组、inode,盘上的组织一层层认过去。第二件,读:一条路径怎么落到盘上的一串块,字节怎么原样回到您手里。第三件,写:往盘上落一个字,牵动的是位图、计数、目录项一连串地方,咱们把每一步的次序走稳。第四件,换根:内存盘光荣退休,真盘上岗,shell 从此活在有家底的机器上。四件之外还有一串随行的家当:块缓存垫在卷和队列中间,任务长出了 cwd,时间戳问主板上的芯片要,连关机这件小事,都学会了走 ACPI 的正门。

门面那一卷咱们交过底:真盘哪天进门,不过是往门面后面再挂一个后端的事。这一卷兑现的就是这句话——ext2 挂上去,客人念的还是路名,柜台添几件业务,其余照旧。也是因为这样,这一卷咱们不赶着造新轮子:位图、睡眠锁、块队列,全是旧相识,力气都花在认字本身。

还有一句复现的实话:机器开机是会往盘上写字的,验收那一章的会话,就在卷上落了一份 note.txt。哪天您想要一卷白纸,把产物目录里的 ext2.img 删掉,再让构建把 ext2_img 这个目标跑一回,新的一卷就烤出来了,盘上的字还是构建期那几样。现在,咱们从卷的版式认起。
