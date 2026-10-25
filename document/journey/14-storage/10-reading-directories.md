---
title: "10 · 目录的念法与 shell 的新手艺"
description: "getdents 一次念一条:slot 的 offset 当条目序号,念到头回 0,念到一条装进 SyscallDirent 再把 offset 加一。号与语义照 Linux,打包缓冲的形状是教学的。shell 添五件新手艺 ls、cat、touch、mkdir、rm,echo 学会重定向;user API 添五个内联包装,两个世界还是读同一张表;hello 光荣退役。"
chapter: 14
order: 10
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - syscall
  - shell
---

# 目录的念法与 shell 的新手艺

目录也是文件,open 打得开。念它走的是 getdents,号是 217,柜台五件新业务里最远的一件。它有个跟 read 完全不同的脾气,咱们直说:一次只念一条。fd 小于 3 拒收,那是标准流的座。缓冲指针越过用户半区拒收。然后翻册子取槽,拿槽里的 offset 当条目序号,问后端要第几号孩子。后端说没有了,回 0,念完了。还有呢,名字和类型装进 `SyscallDirent`,offset 加一,回 1。咱们把这个小结构也摆出来:

```cpp
struct SyscallDirent {
    char          name[cinux::fs::kFsNameMax];  ///< NUL-terminated entry name.
    unsigned char type;                         ///< 0 for a file, 1 for a directory.
};
```

它的字段拢共两个,咱们挨个认:名字占的还是第二章立下的尺寸表,32 格的额度,名字到 NUL 就算收了尾,类型占的是一个字节,0 标的是文件、1 标的是目录,ls 判断要不要补斜杠的时候,依据的也是它。您看,offset 的两种刻度在这里对上了:目录的念,念的就是条目的号,念完一条挪一格,念到哪了全在册子里记着,连 fd 那头都不用多操心。

跟 Linux 的原版比,咱们这里有一处形状上的出入,得跟您交底:真正的 getdents 是把好多条目打包塞进一块缓冲,一次一条是咱们教学的形状。号是原样搬来的,一条一条念的语义也是对的,打包的那层包装等将来的客人真来住的时候再补——号和语义对上了,包装随时能改。

内核这头齐了,shell 那头添手艺。五件新命令,咱们挨个报一遍来路。`ls` 拿 open 打开目录,循环 getdents,一条条往外打,类型是目录的补一道斜杠——屏幕上 `docs/` 带尾巴、`readme.txt` 光着,一眼分清谁装名字谁装字节。`cat` 拿 open 打开文件,128 字节一块循环 read,读到 0 收工。`touch` 最省事:open 时带上创造那一档,close 走人,文件在了,内容是空的。`mkdir`、`rm` 各走各的柜台号,失败时报一声 errno。`echo` 学会了新把戏:词里扫到一个 `>`,后头那个词就是去处,open 创造它,把前头的词一个一个写进去,close 收工。您在验收那一章会看到它落在屏幕上的样子。

客人这头的武器库也扩编了,咱们点名:user/api/syscall.hpp 里添了 Open、Close、Mkdir、Unlink、Getdents 五个内联包装,还是老配方——不带一行运行时,号码直接从内核的头文件里取,两个世界读的是同一张表。卷首立的第二块砖,寄存器的座位,一个字没改:参数还是递进那几个座位,返回值还是正数或者负的 errno,shell 里的 `report_errno` 把负数翻成报错的那一声。

help 报的菜名呢,咱们也看看它,从四样长到了九样:

```text
commands: echo [text...] [> file], ls [dir], cat <file>, touch <file>, mkdir <dir>, rm <path>, help, yield, exit
```

末了咱们办一件伤感的小事:hello 退休。这个名字,首跳那一卷当过验收员,三亿次自增验出了 ring3 的第一跳。柜台开张那章,它写下两行字、办了一次退场。后来 shell 进了构建,hello 就没再上过车。这一卷启动序列直起 shell,树里那行没人编译的死文件,咱们把它请出去了。user/programs/ 从此只剩 shell 一位常客了。

咱们把文件世界的线,从路名一路接到了念目录,到这儿算是接完了。下一章咱们换换口味,看这一卷顺手给全内核的锁立的座次图。
