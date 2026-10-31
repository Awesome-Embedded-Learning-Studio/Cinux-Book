---
title: "02 · 门兑现了它的话"
description: "安全那一卷收卷时撂的话在这里交割:socket 的号位生在那扇门上,新号位出生就在门内,不用写了再改。十四个号位一次挂齐:四十一 socket、四十二 connect、四十三 accept、四十四 sendto、四十五 recvfrom、四十八 shutdown、四十九 bind、五十 listen、五十一到五十五的五个,加二百八十八的 accept4,四十六、四十七两个空着不挂假门。socket 本体是一种文件,跟管道一个待遇:描述符表看见的是一副普通的 FsOps,read 落进 receive,write 落进 transmit,stat 答套接字自己的 mode 位,身份号来自一次开机一份的计数器,无盘起家那一卷传下的旧例。判例一桩:门问描述符是不是 socket,问的是它的 ops 指不指那一副,不指就答 ENOTSOCK,八十八号 errno 新词。诚实拒也在册:流式的类型字答 ESOCKTNOSUPPORT,listen、accept、socketpair 答 EOPNOTSUPP,shutdown 答 ENOTCONN,流式的成员是下一卷的正主,这一卷不冒充。"
chapter: 31
order: 2
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - networking
  - sockets
  - syscall
  - fsops
---

# 门兑现了它的话

安全那一卷收卷的时候,咱们把一句戏排在网络的名下:后面网络那几卷要开的 socket 号位,直接生在指针的那扇门上,新号位出生的时候就在门内,不用写了再改。这一卷开了门,那句话当场兑现了:十四个号位挂上派发表,客人递进来的每一个指针,sockaddr 的地址也好、载荷缓冲也好,走的全是从前那套检查过的抄写,门的代码一行没改,新号位的路径一行没绕。这就是当时把门铺满每一个入口换来的红利,咱们这一卷是头一个消费者。

咱们把号位清点一遍,吃号的顺序照客人的头文件:四十一 socket、四十二 connect、四十三 accept、四十四 sendto、四十五 recvfrom、四十八 shutdown、四十九 bind、五十 listen、五十一 getsockname、五十二 getpeername、五十三 socketpair、五十四 setsockopt、五十五 getsockopt,外加二百八十八的 accept4。四十六、四十七两个座是空着的,那是 sendmsg 与 recvmsg 的座,分批消息的门,这一卷就不挂了,空着的座比挂个假门诚实。挂法还是按号索引的静态表:号落进来,表里有人的话就派发,没人接的话就报一声 unserved 答未实现,老章程咱们一个字没动。

再说 socket 到底是个什么东西。咱们的答话是:一种文件,跟管道是一个待遇的。描述符表看见的是一副普通的 FsOps,不认得也不必认得背后住的是谁。只有 socket 编号的门知道往深里看。外衣咱们照树抄来:

```cpp
const fs::FsOps kOps{
    .lookup     = nullptr,
    .make_file  = nullptr,
    .make_dir   = nullptr,
    .unlink     = nullptr,
    .read       = socket_read,
    .write      = socket_write,
    .read_dir   = nullptr,
    .stat       = socket_stat,
    .retain     = socket_retain,
    .release    = socket_release,
    .readable   = socket_readable,
    .ioctl      = nullptr,
    .adopt_open = nullptr,
};
```

咱们接着看这副外衣怎么当差。read 落进的是 receive,write 落进的是 transmit,readable 问的是有没有东西可收,retention 走的是引用计数。stat 答的是套接字自己的身份:mode 的位是套接字加主人读写,身份号来自一次开机一份的原子计数器,每个 socket 一出生就领了一个,不跟别的重号,无盘起家那一卷给节点发号就是这么办的,stat 问起来答的也是实话。穿好了外衣,socket 就坐进了描述符表,咱们这边 dup 也好、fork 也好、close 也好,描述符表一律拿文件的标准伺候,待谁都是一样的。

门上还立了一桩判例。客人拿一个普通文件的描述符来敲 bind,咱们答什么?咱们答的是八十八号 ENOTSOCK:不是套接字。判的依据就是描述符身上那副 ops:门里的 socket_of 拿到描述符,比对它的 ops 指不指上面那一副 kOps,不指的话当场回绝。有了判例在手,一个管道的描述符、一个盘文件的描述符,谁也别想被咱们当成套接字使,门卫只认的是衣裳,咱们认得又快又准。errno 的词典这一卷添了十二个新词,打头的是 ENOTSOCK,后面的 EADDRINUSE、EMSGSIZE 那一串,都是这个家族自己才知道怎么答的话。

末了是诚实拒的成套,咱们一条条挂出来。类型字要 SOCK_STREAM 的,答 ESOCKTNOSUPPORT:流式的成员是下一卷的正主,这一卷还没有它的位置,不拿数据报的身子冒充流。拒单是这么念的:listen、accept、accept4、socketpair,答 EOPNOTSUPP:这些门的背后还没有那套机制,挂了号位,答出去的话也诚实。shutdown 答的是 ENOTCONN:没有连接,自然谈不上一端一端地断。拒单上每一句都是数着自家货架报的,客人拿到的负数,个个都是有出处的。笔者当年第一遍是反着走的:起手只挂了七个号,后来盘上的客人多起来、配生态的时候才一次补齐十四个。重走的这一遍,侦察是搁在头里的,号位一次生齐了,免了客人中途换表的折腾。
