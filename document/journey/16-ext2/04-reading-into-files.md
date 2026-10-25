---
title: "04 · 把字节念成文件"
description: "lookup 拿 PathCursor 逐组件下爬,同一对纯函数第三回上场。inode 定位是三行算术,hello.txt 的 14 号现场走一遍。找名字和报清单共用一副骨架,rec_len 四道自验,坏卷不硬读。直接十二格、单间到 267、双间到 64 MiB,三重响亮地拒,洞读回来是零。"
chapter: 16
order: 4
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

# 把字节念成文件

读的一切,从一条路径到一张 inode 表的算术开始。ext2 的 lookup,拿的还是名字那一卷那对纯函数:PathCursor 逐组件发放,咱们跟着它一层一层往下走。

```cpp
base::PathCursor walk{path};
std::string_view part;
while (base::NextComponent(walk, part)) {
    const base::Result<unsigned long> kChild = lookup_in_dir(current.value(), part);
    if (!kChild.ok()) {
        return kChild.error();
    }
    current_ino = kChild.value();
    current     = read_inode(current_ino);
    if (!current.ok()) {
        return current.error();
    }
}
```

第三回上场了:ramfs 拿它爬过堆里的树,这一回爬的是盘上的目录,骨架还是原来的那一副,咱们看着它原样服役。查一个名字,就是在目录的块里逐条比对,比对的手艺也是旧的,长度加逐字节,头一棵树那章的远房亲戚。

inode 在盘上哪儿?咱们看三行算术:

```cpp
const unsigned long kGroup = GroupOf(ino, sb_.s_inodes_per_group);
const unsigned long kIndex = IndexOfInode(ino, sb_.s_inodes_per_group);
const unsigned long kBytes = kIndex * sb_.s_inode_size;
const unsigned long kBlock = groups_[kGroup].bg_inode_table + (kBytes / block_size_);
```

咱们拿 hello.txt 的 14 号真走一遍。inode 从 1 起数,一组 128 个:14 减 1 得 13,13 除以 128,商 0 余 13,0 号组、组内 13 号座。盘上一个 inode 256 字节,13 号座合 3328 个字节,表自己的址躺在组描述符里。3328 除以 1024 商 3 余 256,14 号就在表往后数的第 4 个块上、块内第 256 字节起。您拿计算器按一遍,跟咱们算的一个数。每回 lookup、read、stat,都是这么现算现读,内核没给 inode 盖缓存——不盖,重复读靠什么接住?卖个关子,隔一节揭。

目录的条目,两桩事共用一副骨架,咱们照旧合起来讲:找名字是一桩,报清单是另一桩,walk_entries 一副模板吃下两件活。骨架里最要紧的,是每一条在碰字节之前过的四道自验:

```cpp
if (entry.rec_len < kDirEntryHeadBytes || entry.rec_len % kDirEntryAlign != 0 ||
    cursor + kDirEntryHeadBytes + entry.name_len > block_size_ ||
    kDirEntryHeadBytes + entry.name_len > entry.rec_len) {
    return base::KernelError::kIOError;
}
```

rec_len 至少 8,是头的尺寸。4 对齐,是版式排定的。不出块、头加名字不超 rec_len,两条守的是边界。哪道不过,当场 `kIOError`——坏卷大声拒,绝不硬读,跟认卷那几道关一个脾气。还有样东西得交代给您:`.` 和 `..` 在盘上是真条目,真占地方,ls 报它们,跟报别的名字一个待遇。

数据块,15 格 i_block 的分工。文件的第 0 到 11 块,直接查 i_block。第 12 块起进单间,第 13 格指去的 256 格表,管到第 267 块。第 268 块起是双间的地界,第 14 格指外层表,外层表的格子再指内层表,256 乘 256,合 65536 个块,64 MiB,这一版认读的上限。再往后是三重的地盘,`kNotSupported`,响亮地拒。宿主测试里备了案子:20 KiB 的文件踩进单间,525 KiB 的踩进了双间,落点在外层表第二格所指的内层表,验收那一章咱们再对一遍。

最后是洞。查块回来的 0 是洞,那个偏移从来没分配过块。读洞,补零,一个字节不糊弄——ramfs 那一章洞读零,盘上的洞一个待遇。逐块读的路子咱们也直白说了:定位块,算块内偏移,把尾巴夹到文件的真实长度,一块一块搬进您的缓冲。

读到这儿,盘上的字怎么回到您手里,全链路通了。可只读不写,卷就是个标本——下一节,咱们落笔。
