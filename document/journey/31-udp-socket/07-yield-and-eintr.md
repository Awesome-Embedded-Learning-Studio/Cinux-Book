---
title: "07 · 等的门道:让位与被打断"
description: "ping 的收包循环是一路裸阻塞的 recvfrom,等的礼貌全住在门上,咱们把那扇门的等待圈摆开:收着了抄出去答字数。收不着,非阻塞的描述符答 EAGAIN,等着信号来的答 EINTR,都不是就 yield 一圈再来。这套够 ping 用:超时到点调度器记下待送的 SIGALRM,门圈里一眼看见,答 EINTR 出门,门外的投递把信号交进 handler,handler 立旗,ping 收摊报数。Ctrl+C 走同一条路,只是信号换成了 SIGINT。诚实的边界也要立好:yield 是让位不是睡,任务还在调度圈的座位上,一圈一圈醒来看一眼。可打断的真睡、软超时、唤醒的令牌,是下一卷 TCP 的正主,这一卷不冒充。"
chapter: 31
order: 7
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - networking
  - sockets
  - eintr
  - scheduling
---

# 等的门道:让位与被打断

侦察那一章翻译过来的头一行吃法,到这里兑现:裸阻塞的 recvfrom,包不来的话它就不回来,可是超时的 SIGALRM 一到,它就必须是立刻回的。等的礼貌不住在套接字里,家族与环那一章说过的,其实是住在门上。咱们把门上的等待圈原文摆开,摆开的是一圈四步:

```cpp
for (;;) {
    net::SocketAddress from{};
    const auto         kTaken = socket->receive(payload, kWant, &from);
    if (kTaken.ok()) {
        if (!uaccess::CopyToUser(frame.rsi, payload, kTaken.value())) {
            return -kEfault;
        }
        if (frame.r8 != 0) {
            const long long kWrote = write_sockaddr(frame.r8, frame.r9, from);
            if (kWrote != 0) {
                return kWrote;
            }
        }
        return static_cast<long long>(kTaken.value());
    }
    if (kTaken.error() != base::KernelError::kWouldBlock) {
        return errno_of_socket(kTaken.error());
    }
    if ((held->status_flags & kOpenNonblock) != 0) {
        return -kEagain;
    }
    if (signal_pending()) {
        return -kEintr;
    }
    proc::Scheduler::self().yield();
}
```

咱们把圈子的走法过一遍。套接字收着了,载荷抄给的是客人,回信地址抄给的是出参,答的是字数,这是平常的回家。收不着的话,套接字答的是 would-block,门看客人的意愿:描述符生下来带着非阻塞位的,门是不陪等的,答的是 EAGAIN,这个世界里是没有东西可读的,话说完了就走。没带的,门再看自己:有信号候着吗?候着的话就答 EINTR,调用是被信号打断的,这句 errno 不是套接字家族的新词,睡的门那几卷早就立过,这里原样使的就是它。都不是的话,咱们让位一圈,把处理器交给别的任务,下一圈轮回来再收一次就是了。

咱们拿 ping 的两个时刻把等待圈走活,到了大戏那一章,您都会亲手敲到。头一个时刻是超时:客人起了 setitimer,两秒到了点,调度器在定时器的扫描里把 SIGALRM 记到待送的名册上。ping 还在圈里一圈圈地让位,下一圈进了门,signal_pending 一眼看见名册上有自己的信号,答的是 EINTR,出了门。门外的章程是老的:每个号位办完事,派发的汇都要问一遍有没有信号要投,投递把 SIGALRM 交给了 handler,handler 里立的旗就是 ping 的收摊旗,ping 的主循环看见旗,跳出了等待,报它的数,几发几收是一目了然的。第二个时刻是打断:控制台上 Ctrl+C 按了下去,终端把 SIGINT 记上前台组的待送名册,往后的路与超时一个模子,连答的话都是同一个 EINTR。ping 的两条命脉,一条牵的是超时,一条牵的是打断,全在等待圈的第四步上汇合,这就是侦察时说的等待要能被信号打断,门上的写法就这四步,一步也是不多的。

边界的话也立在这儿,咱们不藏。yield 做的是让位,不是睡:让位的任务还在调度圈的座位上,一圈一圈地醒,醒一次看一眼环上的货,没有的话再让出去,处理器是没有闲着的,别的任务也没饿着,不过这样等,是拿调度圈的钱买的。真睡走的是另一门手艺:任务从座位上退下来,睡在自己的上下文里,环里到了货,就有人拿着令牌来唤醒了,醒的是中断或者泵,而不是调度圈。再加上软超时的活,等满了约定的时间就自己醒。这三样是下一卷的正主,流式的等待处处要用,到了那一卷一起立。这一卷的门只会让位,它会什么咱们就如实说什么,冒充的事咱们不干。法子是朴素了点,对客人是够的:ping 是一个人在等,机器上没有别的忙人,让位一圈的成本,它是付得起的。
