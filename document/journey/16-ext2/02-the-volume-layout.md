---
title: "02 · 卷摊开来的样子"
description: "头 1024 字节留给引导块,superblock 坐 2 号扇区。卷切成块组,每组自带两张位图和一张 inode 表,教学卷 4 MiB、1 KiB 块、128 个 inode,整卷就是一组。座位表拿 static_assert 压进编译期:superblock 1024 字节、组描述符 32 字节、inode 128 字节,一格不许差。"
chapter: 16
order: 2
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

# 卷摊开来的样子

版式这一课,咱们从卷的头一个字节认起。ext2 的卷,头 1024 个字节不归文件系统管,那是留给引导块的地界,启动程序要是住在那儿,卷自己不搅扰。superblock 从 1024 字节的地方起坐,按扇区数正好是 2 号:内核想认一卷盘,您猜头一件事是做什么?去读 2 号扇区。

superblock 有 1024 字节,组描述符 32 字节,inode 128 字节,目录项的头 8 字节——这几个数,咱们拿 static_assert 压进编译期:

```cpp
static_assert(sizeof(Superblock) == 1024);
static_assert(sizeof(GroupDescriptor) == 32);
static_assert(sizeof(Inode) == 128);
static_assert(sizeof(DirEntry) == 8);

static_assert(offsetof(Superblock, s_magic) == 56);
static_assert(offsetof(Inode, i_block) == 40);
static_assert(offsetof(Inode, i_mtime) == 16);
```

座位表这门手艺,推开寄存器窗那一卷刚上过一堂,这一卷咱们接着用。盘上的版式跟设备的寄存器一个脾气,差一格,读到的就是邻居家的东西。superblock 里咱们用得上的字段,顺手认几个。s_inodes_count 报 inode 总数,s_blocks_count 报块总数。s_log_block_size 说块多大,1024 左移它这么多位。s_magic 坐第 56 字节,读出来得是 `0xEF53`。还有一格得点名:s_inode_size,盘上一个 inode 实际多大,它说了算。

卷再大,家当不能全堆在卷头。ext2 把卷切成一块一块的组,每一组自带一套:块位图,记组里哪些块空着。inode 位图,记哪些号空着。inode 表,一位一个 inode 排成一排,一位多宽,superblock 说了算。这三处的址,外加空块空号的计数,收在 32 字节的组描述符里。全卷的组描述符排成一张表,紧挨着 superblock。咱们手里这卷教学盘,4 MiB 合 4096 个块,一组至多装 8192 个块——整卷,就是一组。

inode 值得咱们单独多看两眼。盘上 inode 的实际尺寸,以 superblock 为准,咱们结构体认的是 128 字节的下限——mkfs 烤的这卷,报的是 256 字节一个,多出来的地界记着咱们不读的扩展字段。头 128 字节里,i_mode 说它是文件还是目录、什么权限。i_size 说它多大。i_links_count 数几个名字指着它。i_mtime 记它最后一次改内容是哪一秒。最要紧的是 i_block,十五个格子:头十二格直接放数据块的号,第十三格放一张间址表的址,第十四格放两张套起来的表,第十五格留给三重间址——三重这一层,咱们这一卷不认。

间址表长什么样?咱们算一算:一张表占一个块,一块 1024 字节,一格 4 字节,正好 256 格。单间,就是 i_block 第 13 格指着一张 256 格的表,文件的第 12 到 267 块归它管。双间再套一层,256 乘 256,合 65536 个块,64 MiB,这是这一版认得动的上限。目录项是另一副身板:8 字节的头,inode 号、rec_len、name_len,名字跟在头后面。rec_len 说的是下一家在几个字节之外,目录的条目就靠它一节一节串起来。

末了咱们把整卷摊在桌面上数一遍:引导块 1 个,superblock 1 块,组描述符表连一块的零头都占不满,两张位图各占 1 块,128 个 inode 按 256 字节一个摊开,表要占 32 块。卷头的家当拢共 37 个块,余下 4000 个出头,全是数据的地界。4 MiB 的卷在咱们手里,就这么大一点,每一块住谁,数得出来。

版式摊开了,可长得对的卷,咱们还不一定认得。进门要验——下一节,认卷之门。
