---
title: "01 · 头一件事:看客人要什么"
description: "socket 是给客人用的,动工之前咱们把客人摸一遍。盘上的 busybox 是 1.38 的,网络那几个小程序全编进去了:ping、ping6、ifconfig、netstat、nslookup、nc、telnet。挨个看它们的吃法:ping 亲口说 ICMP,自己构回声请求自己解回声应答,收包的循环是一路裸阻塞的 recvfrom,超时靠 alarm 与 setitimer 到点送 SIGALRM,打断靠 SIGINT 的 handler。号位吃的是人家头文件里排好的表,四十一到五十五加二百八十八,sockaddr 的形是十六字节。nslookup 只要 poll 加 read 加 write 走一个描述符,用户态自己就够。侦察下来活分四摞:UDP 模块、socket 的门、定时器、裸套接字,setitimer 是硬前置。SLIRP 没有 UDP 的 echo 服务,当年的原话还挂着,所以收官的大戏是 ICMP 的 ping,UDP 的真服务器验收排在 DNS。"
chapter: 31
order: 1
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - networking
  - busybox
  - recon
---

# 头一件事:看客人要什么

这一卷的正主是 socket,socket 生下来是给客人用的。所以动工之前,咱们把客人请到桌上,挨个问一句:你吃几号菜,什么口味。客人就是盘上的 busybox,从开门迎客那一卷起它就是 shell 的本体,1.38 的版本,网络这一档的小程序当时就全编了进去:ping、ping6、ifconfig、netstat、nslookup、nc、telnet,七个全到齐了。咱们这一卷要伺候的就是它们。

咱们从 ping 读起,它是收官大戏的主角。读它的收包循环,吃法就摆在那儿:socket 要一个 SOCK_RAW 的,协议字填的是 IPPROTO_ICMP,回声请求是自己构的,回声应答是自己解的,来回的秒数自己掐。收包的那一行,是一路裸阻塞的 recvfrom,包不来的话它就不回来。超时怎么办?它起手一个 alarm 或者 setitimer,到点内核送来的是 SIGALRM,handler 里立了旗收摊。打断怎么办?SIGINT 的 handler 干的同一件事。咱们把这三行吃法翻译成自家的活:裸阻塞的 recvfrom,意味着门上的等待要能被信号打断,打断的答话得是 EINTR。到点的 SIGALRM,意味着 alarm 与 setitimer 是绕不开的硬前置,缺了它们 ping 连超时都不会有。到了 SOCK_RAW,咱们还得给 ICMP 开一扇不穿 UDP 词书的直通道。

> 笔者当年第一遍把顺序走倒了:内核里起手立了一个自家的 ping 号位,壳里敲 ping 走的是内核的门,SOCK_RAW 是后来盘上的客人多起来、配生态的时候才补上的。重走的这一遍,内核那步省了,直接上的就是 SOCK_RAW,一步站到客人的位置上验收。

号位的事,咱们照开门迎客那一卷立下的北极星办:人家的表怎么排号,咱们就怎么挂。人家头文件里 socket 占的是四十一到五十五这一段,加一个二百八十八的 accept4。中间的四十六、四十七两个是 sendmsg 与 recvmsg,分批消息的门,这一卷就不挂它们了,空着的号位比挂个假门诚实。地址的形也对上了:sockaddr_in 十六个字节,头两字节放的是族,接着两字节的端口、四字节的地址,端口与地址在线上都说大端。这些是客人进门就要按的 ABI,咱们照着给就是。

nslookup 的吃法另有一番清爽。它 poll 的是一个描述符,read 与 write 走的都是文件的那一副脸,也就是说 poll、read、write 三个号位早在前面几卷就位了,只要 socket 真的穿文件的外衣,它用户态里自己就够了,咱们不用为它在内核里加一行。ifconfig 的吃法咱们留到它那一章当面看,这里只记一句:它问话走的是 ioctl 的字,就不用另开号位了。

大家猜一猜,七位客人的吃法摸下来,活会分成几摞?侦察到此,活就分成了四摞,次序也定下了。头一摞装的是 UDP 模块,四层的座上给它留一个位。第二摞是 socket 的门,十四个号位一次生齐了,socket 本体穿文件的外衣坐描述符表。第三摞收的是定时器与打断的成套:alarm、setitimer、EINTR,外加两笔顺手到期的欠:一笔欠在描述符表的引用上,一笔欠在退出路径的栈上,到了表的那一章一起还。第四摞是 SOCK_RAW 与 ICMP 的直通道。setitimer 是排在硬前置里的,客人的超时全指着它。还有一句边界的话在这儿立好:当网关的那个服务,当年实测的口径是它没有 UDP 的 echo 服务,原话倒是不期望 reply,咱们不替它说话。所以这一卷的大戏是 ICMP 的 ping,UDP 的成色要在真服务器上验,等到 DNS 那一章就有了,网关后头真的坐着一位答话的,到了那儿再会它。
