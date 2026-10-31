---
title: "12 · 壳里的第一声 ping"
description: "收官大戏开锣:壳里敲 ping -c 3 -W 2 10.0.2.2,两颗核的机器,三发出门,面板上落 bytes from 10.0.2.2 与 3 packets transmitted,提示符照常回来。这一声 ping 的每一环都是这一卷的活:裸套接字进门,sendto 上真线,泵排水,观察哨交货,recvfrom 的圈收包。两幕配戏也各就各位:-W 1 敲一个不应的地址,SIGALRM 到点,EINTR 出圈,统计落款带出 1 packets transmitted, 0 packets received, 100% packet loss 一行。不限发数的 ping 敲下去,一声 Ctrl+C,泵翻字,前台组收 SIGINT,统计落屏,提示符回来。当网关的服务没有 UDP 的 echo,边界照旧念:大戏是 ICMP 的,UDP 上真服务器,排在 DNS 那一章。"
chapter: 31
order: 12
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - networking
  - ping
  - busybox
  - icmp
---

# 壳里的第一声 ping

点亮网卡那一卷的第一声 ping,是咱们自己的 kernel_init 发的,自家孩子自家考。这一卷的大戏换主角:壳里,客人亲手敲。机器照收官那天的排面起着:两颗核在线,壳的提示符候着。轮到您把命令敲进去:

```text
/ # ping -c 3 -W 2 10.0.2.2
```

三发,每发等两秒。这一行命令进门之后走的每一步,都是这一卷搭的路,咱们从头走一遍。socket 要 SOCK_RAW 与 ICMP,euid 是零,门放行,裸套接字落在观察哨的名册上。sendto 头一发上真线,十点零点二点二是网关,路由查名册,头一发照例花在问路上,ARP 的广播出门,这一段是点亮网卡那一卷的老戏。泵一圈圈排水,应答落环,汇判两次,哨位交货,裸套接字的环里躺进了连头的完整数据报。壳里的 ping 此刻正阻塞在 recvfrom 的圈里让位,包一到,圈里收着,回信地址抄进出参,门答字数,ping 解开二十字节的 IP 头,读到类型字零,回声应答,记一笔收。setitimer 备的两秒钟每一发都没用上,回音来得快。头一行回音落屏的那一下,咱们停下来把三发等完,才去看断言:bytes from 10.0.2.2 的回音行,3 packets transmitted 的总数行,外加提示符照常回来。当年第一遍,笔者把这一幕的路分两段才接通,侦察那一章的旧话记过:起手壳里能敲的 ping 是自家的小命令,走内核替它扛的门。客人的 ping 等到 SOCK_RAW 补上才在用户态自己站住。重走的这一遍,客人自己站着把戏唱完了。

第二幕,超时。咱们换 -W 1,等一秒,地址换一个不应话的:

```text
/ # ping -c 1 -W 1 10.0.2.254
--- 10.0.2.254 ping statistics ---
1 packets transmitted, 0 packets received, 100% packet loss
```

这一幕咱们考的是钟。setitimer 起了一秒的钟,ping 阻塞在圈里,一秒到点,调度器的扫描把 SIGALRM 记上名册。下一圈进门,signal_pending 看见,EINTR 出圈,门外的投递把信号交进 handler,旗一立,ping 收摊,统计的落款下来,零收的总数一行落屏。一秒的钟,一圈的让位,一声到点,一处不差。

第三幕,打断。发数不限的 ping 敲下去,回音一行一行地滚,这时候咱们把手指按上 Ctrl 与 c:USB 的报告进门,修饰字节里 Ctrl 的两位立着,c 的字节算成三。泵摇着终端,行纪律认出 VINTR,翻成 SIGINT,记待送,送前台组。壳在前台,可壳正等着 ping,ping 阻塞在圈里,组的信号落在它的名册上。下一圈 EINTR 出圈,handler 立旗,ping 打出统计,统计的落款咱们断言守着 ping statistics 与提示符两样。从手指到统计,四段路,合起来的是三个卷的活:组的根基是听得懂喊声那一卷立的,组到前台组的键是假文件与真终端那一卷接的,键的翻译与圈的礼貌是这一卷的。

边界的话咱们收尾再念一遍,不因为它喜庆就咽下去。当网关的那个服务,当年实测的口径是它答 ARP、答 DHCP、答 ICMP 的回声,UDP 的 echo 没有,原话是不期望 reply,这句话的来路,答话人那一章有原样。所以大戏请的是 ICMP 的 ping。UDP 要在一位真的答话人面前验成色,得等 DNS 那一章,网关后头真的坐着一位。眼下的舞台上,客人自己发、自己收、自己超时、自己被打断,四门功课齐了。
