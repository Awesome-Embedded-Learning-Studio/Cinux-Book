---
title: "15 · 线那头的答话人"
description: "UDP 的成色终于有人验了:网关后头坐着一位答 UDP 的,DNS,十点零点二点三的五十三口。种子在盘里:/etc/resolv.conf 一行 nameserver 10.0.2.3,/etc/hosts 两行 localhost 与 cinux,造盘的脚本从种子的目录里装进去,读者重造盘就带着。nslookup 不抄 hosts 的近道,点名问服务器:socket 要数据报的,sendto 把查询递上真线,套接字悄悄领一个短命号码落座。查询进对面的五十三口,应答穿回 e1000、泵、汇、三重门、端口表,落进短命号码的环,recvfrom 圈里收着,poll 加 read 加 write 全在用户态走完,内核一行没为它加。面板落 Server 与 Name 与 Address 三行断言。卷首撂的话在这里兑现:当年笔记的原话落回正文,SLIRP 无 UDP echo 服务,不期望 reply,echo 今天仍没有人答,可它有 DNS,新测出的答话人,等的原来是它。"
chapter: 31
order: 15
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - networking
  - dns
  - udp
  - ext2
---

# 线那头的答话人

卷首立边界的时候咱们说过:当网关的服务没有 UDP 的 echo,所以大戏的角色是 ICMP 的 ping,UDP 的成色得等一位真的答话人。这句边界有出处,不是猜的。当年第一遍送 UDP 上真线的那天,笔记里记的原话是:SLIRP 无 UDP echo 服务,不期望 reply。那一发蹭的是 ping 刚验过的出门路,只证包能从真线出去,没等回音。点亮网卡那一卷记下的边界,是它答 ARP、答 DHCP、答 ICMP 的回声,TCP 与 UDP 的 echo 服务没有,握手的应答没测过,不替它说话。这几样里,ARP 与 ICMP 的回声两样,点亮网卡那一卷在真线上亲手验过,DHCP 那一笔是当年的记录收进正文的,咱们没走过那道手续。可这一卷的实测添了新的一笔:同一位网关的后头,坐着一位答 UDP 的,就是 DNS,十点零点二点三,五十三口。它是这一卷实测出来的答话人,不是替 echo 填空,UDP 等的这个人,这一章等到了。

家当是两份种子文件,住在盘的 /etc 里。resolv.conf 一行, nameserver 10.0.2.3。hosts 两行, 127.0.0.1 localhost 与 10.0.2.15 cinux。装进盘里的路是造盘脚本铺的:种子住在仓库的 scripts/ext2_seed 目录里,造盘的时候装进 /etc。读者重造一遍盘,两份就原样带上,种下的每一行开机可验。咱们看,两份各有各的客:hosts 是给走 libc 解析的客人抄的近道,名字在本地就能对上号。nslookup 偏不抄这个近道,它点名问服务器,resolv.conf 里写的是谁,它就问谁。

咱们把 nslookup localhost 一行敲下去,顺着包走一遍。客人在用户态构好查询,socket 要一个数据报的。sendto 把查询递出来,目的地十点零点二点三的五十三口。套接字没 bind 过,发送的路上悄悄领一个短命号码落座,回信有处投递。查询上了真线:路由看地址不在回环网,递给网卡,ARP 的名册问一趟路,虚拟网络那一头应了门,帧出了门。应答往回走:e1000 的收环接住,泵排的水把它送进汇,汇判两次,协议字十七,UDP 的座接包。三重门一道道过,长度核对、校验和连伪首部折拢。端口表查到短命号码,载荷连同回信地址落进环里。壳里的 nslookup 此刻阻塞在 poll 加 read 的用户态循环上,read 落进套接字的 receive,环里弹出,答案到手。这一路里内核一行都没有为 DNS 加过:poll、read、write、socket,全是这一卷与前头各卷立好的门,客人穿着文件的外衣把事办完,这正是卷首侦察时记下的那句,nslookup 用户态自足。

面板上落的,咱们断言守着三样:Server 一行,点名 10.0.2.3。Name 一行,localhost。Address 一行,127.0.0.1。名字问的是对面真服务器,答案也是从线上答回来的。回程的线还有一层意思值得点破:三发 ping 走的是裸套接字的直通道,不穿词书。这一包 DNS 应答走的是 UDP 的全套,词书、三重门、端口表、短命号码、环,一件不缺。这一包应答进环,UDP 的全套家当才算当面验过成色。客人名单上七位,验过成色的三位:ping 的裸通道,ifconfig 的名册,nslookup 的全程。剩下的几位客人,telnet 与 nc 要等流式的成员,netstat 要等的数还没立,ping6 问的是 v6 的路,那套栈还没立,名单留着,戏排在后头。
