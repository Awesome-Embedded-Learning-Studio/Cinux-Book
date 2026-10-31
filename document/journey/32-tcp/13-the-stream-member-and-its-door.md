---
title: "13 · 流式的成员进门"
description: "上一卷门后那句诚实拒在这一章兑现:类型字要流式的,门后站着真的 TcpSocket,协议字点六或留空,别的不认。成员一张票两态:听着的时候攥听票,开了连接攥连接票,accept 落下的新套接字出生就是已开的连接,对家从模块问来。listen 与 accept 真分派,accept 的等待没有期限:圈还是上一卷那四步,候客座空答 would-block,非阻塞的描述符不陪等,答 EAGAIN,有信号候着答 EINTR,都不是就让位一圈。accept4 的旗归新生的描述符,听者自己的非阻塞位才决定等不等,旗认生面孔答 EINVAL。connect 三态:被拒答 ECONNREFUSED,七十五秒软限到了还在握答 EINPROGRESS,通了答零。shutdown 要一:一送告别信,别的答不支持,过二答无效。sendto 三型限额,流式一个最大段,短写是窗的常态。上一卷撂的真睡三样,这一章如实对单:到点叫人的钟立了,任务侧的软限在 connect 门里开了一回张,睡进自己上下文与令牌唤醒,续欠。"
chapter: 32
order: 13
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - networking
  - tcp
  - sockets
  - syscall
  - accept
---

# 流式的成员进门

协议层的活到上一章齐了,可客人的手里还是拿不到:客人拿的是描述符,不是连接票。这一章咱们把流式的成员送进套接字的家族,上一卷门后那句诚实拒在这里兑现:类型字要流式的,门后站着真的东西了,协议字点六或留空,别的不认。socketpair 照旧答不支持,那扇门后没有东西,空着比挂假门诚实,老话不改。

咱们看成员的长相,一张票两态。TcpSocket 出生的时候什么都不是:bind 记下自家的口,listen 换一张听票,攥在模块的听座上。connect 换一张连接票,连着模块的连接座。两态不并立,听着就不能再去连,连着就不能再听。accept 落下来的新套接字最省心,出生就是已开的连接:候客座里的票递过来,对家的地址从模块问一句,新成员不握手,握手是爹娘在协议层早就办完的事。它在描述符表里就是一件普通的文件,read 落进收,write 落进发,外衣是上一卷缝好的那件,一针没改。

listen 与 accept 背后那套机制,上一卷收卷时点名要立的,咱们看 accept 的门怎么等。等的圈子是上一卷 recvfrom 那四步,原样搬来:

```cpp
        if (kTaken.error() != base::KernelError::kWouldBlock) {
            return kTaken.error() == base::KernelError::kNotSupported
                       ? -kEopnotsupp
                       : errno_of_socket(kTaken.error());
        }
        // accept4 flags apply to the new descriptor. Only the listener's
        // O_NONBLOCK decides whether an empty queue may wait here.
        if ((held->status_flags & kOpenNonblock) != 0) {
            return -kEagain;
        }
        if (signal_pending()) {
            return -kEintr;
        }
        proc::Scheduler::self().yield();
```

引文里那半句特判,咱们交代明白:accept_one 递回来的错读出 kNotSupported,门答 EOPNOTSUPP,不走 errno_of_socket 的公用表。有它的道理:公表把 kNotSupported 映成 EMSGSIZE,那个号是给 UDP 大包超限预备的。listen 与 accept 这两扇门共用公表就成了误报,所以两处各自特判,照实答操作本身不支持。候客座是空的,答 would-block,门看意愿:描述符生下来带着非阻塞位的,不陪等,答 EAGAIN。有信号候着的,答 EINTR,门外的投递照老章程办。都不是,让位一圈,把处理器交出去,下一圈回来再看。没有期限:客人挂一个口在后台等人上门,等多久都该等,期限是咱们替人做的主,不做。accept4 的两个旗要分家,注释里的原话说得明白:旗归新生的描述符,听者自己那身非阻塞位才决定门里等不等。旗里认出别的位,答 EINVAL,生面孔不装熟。

connect 的门里立了这一卷唯一一处任务侧的软限,咱们把三态念齐。门头一件事是递 SYN 出去,然后一圈一圈地等:对端回了 RST,握手失败,答 ECONNREFUSED,门没开就是没开。七十五秒软限到了还握着手,答 EINPROGRESS,连接还在路上,客人拿这个号问进程也好等拨号音也好,是客人的事。通了,答零。软限这个数不是拍的,回环上一瞬就通,真线上跨洋一百多毫秒,重传五次花满也就一分钟的量,七十五秒罩得住,又不至于把死等的客人吊太久。

shutdown 这一回有真话可答。参数要一,送告别信,close 的路数,半闭是这一卷真给的。答二收向的关,不支持,收的路由对端的信说了算,咱们不冒充。过了二,EINVAL,数的范围都不对。sendto 的限额按出生的类型分三档:裸的一千四百八十,流式一个最大段一千四百六十,数据报一千四百七十二。流式的限额最小,短写在这里是家常便饭:窗就那么宽,一笔递不下的分几笔,字数如实报,客人按数续递,这正是流的语言。

上一卷撂过一句整包的话:可打断的真睡、软超时、唤醒的令牌,排在下一卷。现在下一卷写到了门口,咱们对单。钟立了,内核侧的等待吃上了它,重传与多留一程都在钟上活。软超时在 connect 的门里开了一回张。可圈还是让位的圈:任务还在调度圈的座位上,一圈一圈地醒,睡进自己的上下文、有人拿令牌来唤醒,这两样没立。咱们不把话说满,单子续期,挂在明处。长凳三案守着这一章:回环口上一场完整的会话穿过 accept。挂上真网卡再验回环的握手与字节,照旧,这一案是下一章那桩案子结了之后回头补上的,时序上它排在后头,名字记在这里。写的人早走一步,套接字销毁递的是道别不是撕脸,计数器里 RST 一笔没记,候客座里的字节与流尽替他守着,accept 领人,读到最后一笔还有货,读完答零。死讯的发送半边顺手交代:自家的告别信递了,发送的路就算打烊,客人还往里递字,门答 EPIPE,管子断了。收的方向答流尽,发的方向答断管,与道别那一章报被拒是同一口径:死讯不走回调,挂在票上,哪头的门被问,哪头如实答。还有一案专考旗的分家,accept4 的旗落在新描述符的身上,听者不带。成员进门,戏台齐了。
