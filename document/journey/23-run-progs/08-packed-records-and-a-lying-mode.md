---
title: "08 · 打包的记录与说谎的 mode"
description: "getdents 从一次一条改成打包式:字段按偏移常量落笔,整条八字节对齐,续读的饼干写明用途,自家 shell 的 ls 改成解包循环。ioctl 对 fd0 到 fd2 答零 termios,新词 Enotty。裸命令稳定报 Permission denied,排查收进一个矛盾:13 出在用户态,execve 却没进过内核。定案:stat 的教学四十字节对不上 musl 要的一百四十四,假 mode 恰好没有执行位,ash 在 stat 处自判了拒绝。"
chapter: 23
order: 8
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - filesystem
  - abi
  - debugging
---

# 打包的记录与说谎的 mode

咱们头一件改的是目录的念法。无盘起家那一卷立 getdents 的时候,咱们一次念一条,offset 当条目的序号,念完了加一,缓冲的形状是自家教学的。真客人期待的是 Linux 的打包式:一个缓冲里挤多条记录,每条带着自述的长度,念到放不下的地方为止,下次从上次停的地方接着念。字节计划咱们按偏移常量落笔:

```cpp
inline constexpr unsigned long kDirent64InoAt  = 0;
inline constexpr unsigned long kDirent64OffAt = 8;
inline constexpr unsigned long kDirent64LenAt = 16;
inline constexpr unsigned long kDirent64TypeAt = 18;
inline constexpr unsigned long kDirent64NameAt = 19;
```

为什么不用结构体:名字是变长的,C 里挂在结构体尾巴上的柔性数组成员是扩展写法,咱们不让编译件吃它,而正经结构体的自然对齐又会在十九字节的名字前把头垫到二十四,线格式就错了。所以落笔认的是偏移,不劳结构体的驾,这段道理写成了教学注释留在词表里。续读的饼干写的是条目的序号,下一回从它指的地方接着念。类型词换成了人家 hosted 的值,文件给的是八,目录给的是四。目录条目里添了 inode 号的格子,ext2 填的是真号,内存的假盘留零。改造走的是替换:用户侧的 Getdents 升成三参,自家 shell 的 ls 改成解包循环,一条条地按自述长度往前走,咱们拿标准盘实弹,ls 照常过了,老消费者重编一遍就搬了家。两头的约定也照人家的:目录念尽了返回零,ls 的解包循环就靠这个零收步。缓冲小得连一条记录都装不下的,答的是 Einval,咱们不硬塞半条。

咱们再开一扇问话的门,ioctl 的十六号。头一问是 TCGETS:对 fd0 到 fd2 答三十六字节全零的 termios,别的号与文件的座,答的是 Enotty,errno 名册添的是二十五号新词。isatty 探的就是这一号:ash 靠它判断要不要开作业控制。当时咱们以为这就够用了,这话记下了,后面亲手一玩的时候再验一次。

然后是这一卷最隐蔽的一案。TCGETS 落地之后,PATH 形态的裸命令,busybox、ls 这样的名字,稳定地报 Permission denied,权限上的拒绝,errno 的十三号。绝对路径倒是全通,/bin/busybox uname 好好的。咱们一步一步排除:TCGETS 的开关二分验证,排除了它。内核的 errno 表里,根本没有能答出十三号的来路。execve 的 handler 上断点,断了个空,连分派的总闸上按号数条件断,也扑了个空,咱们断定 execve 压根没被叫过。咱们的 unserved 探针一声不响,被敲的空号一个也没有。线索收拢成了一个矛盾:十三号产生在用户态,musl 的 execvp 见了 EACCES 会攒着,这是已知的出处,可 execve 没进过内核的事,偏偏跟它对不上。疑点转向了 ash 自己的搜索前置检查。案卷又送了出去,请那双眼睛又看了一遍,定案倒是快得很。

真身:stat 的返回结构错位。咱们的 SyscallStat 是五个六十四位字段、四十字节的教学形状,musl 按 Linux x86_64 的一百四十四字节读。它的 mode 落在偏移二十四,占的是三十二位,咱们的 mode 写在偏移八,占的是六十四位。于是 musl 读到的 mode,其实是文件大小 1214352 的低三十二位:0x128790。这个假 mode 碰巧长得像普通的文件,可执行位偏偏与不上:0x128790 按位与 0x49,得的是零。ash 找命令的章程,是对 PATH 的候选挨个 stat 一遍,查的是类型与执行位,查不过,当场自判了 EACCES:fork 与 exec 根本没开始。带斜杠的命令跳过候选检查,所以绝对路径全通。硬件断点抓的现行,咱们原样抄来:

```text
ASH_TEST_EXEC mode=0x128790 IX=0 size=288230513590665216
/bin/sh: line 0: uname: Permission denied
```

您看那个 size 也是被错位读出来的天文数字,一行报错里的赃物有两处。hello 的下场更惨,它的假 mode 是它的大小 0x3788,连普通文件的类型检查都过不了。

修复分了三层。SyscallStat 改成 Linux x86_64 的完整一百四十四字节,字段宽度定得死死的,宿主的案子直接拿系统的 struct stat 对表,断言把 ABI 定住:几条关键的抄在下面。咱们让 stat、lstat、fstat、newfstatat 四个入口一律清零整份记录再填已知字段,输出范围咱们按一百四十四字节整查。咱们把内部摘要的 FsStat 原样保留,教学的那点简化住在内核里,谁也不管它长什么样,可跨过 syscall 边界的那一刻起,它就是线格式了,得按线上的约定来。

```cpp
static_assert(sizeof(SyscallStat) == 144, "Linux x86_64 stat is 144 bytes");
static_assert(offsetof(SyscallStat, mode) == 24, "stat mode is a 32-bit word at +24");
static_assert(offsetof(SyscallStat, size) == 48, "stat size sits at +48");
```

另有一笔独立修:openat 与 newfstatat 的描述符参数按六十四位比较,零扩展的 AT_FDCWD,0x00000000ffffff9c 被误拒了,改成了按三十二位带符号解码,零扩展与符号扩展的两种传参都有案子盖着。边界咱们也如实交代:设备号、访问时间、块数这些摘要还供不出的,暂且答的是零,uid 与 gid 对着单用户的 root。还有一句要嘱咐您:旧四十字节时代编的教学 shell,配新内核是跑不了的,旧二进制读新记录:字字都是错的,重编了再上。

咱们修完,同一断点再看了一遍,mode 读回的是 0x81ed,正是普通文件带执行位的形状,IX 一栏报的是 0x49:

```text
ASH_TEST_EXEC mode=0x81ed IX=0x49 size=1214352
path-uname-status-0
```

裸 uname 报出了 Cinux,PATH 的裸查找全通,二十连发照旧全过了。不过这一轮的复跑现场,替 ash 备着找路的家什,真到开机直拉的 ash 手里,envp 是空的,裸名字是没处找的,路名的事下一篇接着办。这一案与 stat 的座次案是双生的两胎:一个错的是号,一个错的是形状,病根倒是同一条:自家对齐的还是自家,外头客人手里的 ABI 不跟咱们商量,绿了多少年都不算数。跨边界结构的教学简化是隐性的双胞胎旧病,这一卷咱们在同一处栽了两回,咱们把它记下,当的是一句家训。
