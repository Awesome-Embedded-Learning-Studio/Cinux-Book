---
title: "08 · 给 ping 备的钟"
description: "ping 的 -W 全指着到点的 SIGALRM,三扇门为它开:三十六 getitimer、三十七 alarm、三十八 setitimer。三样定时器里只实装真时的 ITIMER_REAL,另两样答 EINVAL。Itimerval 的 ABI 三十二个字节,间隔在前值在后,秒与微秒各占八字,换算成纳秒时负数、微秒过百万、加法溢出都答 EINVAL,换算回来微秒向上取整。alarm 答旧钟的余秒,同样向上取整。钟挂在进程的组长身上,按 tgid 找人,fork 出来的孩子自立门户,exec 换了戏服钟还在,退场了钟就哑。到期走扫描:一次性的清零,周期性的相位前移,错过的拍子并成一声信号。SIGALRM 在信号门内记一笔,明写过 SIG_IGN 的不吵人家。投递不走通用的送信,不叫醒睡死的任务。下一次到点的下界是原子量,钟摆的快路径一读便知。"
chapter: 31
order: 8
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - timing
  - itimer
  - signals
  - sigalrm
---

# 给 ping 备的钟

侦察那一章排的硬前置,这一章兑现的就是它。ping 报出的是 -W 2,意思是两秒听不见回音的话就收摊,收摊的令牌是一声到点的 SIGALRM,送钟的号位有三扇:三十六 getitimer、三十七 alarm,再加三十八的 setitimer。人家内核里的定时器有三样脾气,真时的、虚时的、profiling 的,咱们实装的是真时的 ITIMER_REAL,记的是墙上的钟,任务睡不睡它都走它的。另两样答的是 EINVAL,没有的东西不装。

ABI 的形,咱们头一件事就对齐它。Itimerval 的形是三十二个字节:前十六个是间隔,后十六个是当前的值,每一段里秒与微秒各占的是八字。间隔在前值在后的次序,是人家 ABI 的原样,抄的时候原样落笔。进门的三样验算也严:秒是负的,微秒是负的或者到了一百万,换算成纳秒的路上加法要溢出的,统统答的是 EINVAL,糊话是不收的。往回想换算的时候,微秒是向上取整的:还剩一微秒,也得把人家的一微秒答回去,向下抹零就是拿人家的余量冒充走完的钟。alarm 的答数同款讲究:起一口新钟,间隔递的是零,值是客人递的秒数乘十的九次方纳秒。旧钟还剩的余量换算回秒,同样是向上取整了再答回去。咱们把这扇门的原文摆出来:

```cpp
long long HandleAlarm(const SyscallFrame& frame) {
    proc::RealTimerValue timer{
        .value_ns = static_cast<unsigned int>(frame.rdi) * kNanosecondsPerSecond,
    };
    proc::Scheduler::self().real_timer(*CurrentTask(), MonotonicNanoseconds(), timer, true);
    return static_cast<long long>((timer.value_ns / kNanosecondsPerSecond) +
                                  (timer.value_ns % kNanosecondsPerSecond != 0 ? 1 : 0));
}
```

real_timer 出来的时候,timer 里装的就是旧钟的余量,门的答话拿它取整,一来一回都是清清楚楚的。钟挂在谁的身上,这笔讲究咱们得讲清楚:挂的是进程的组长。real_timer 进了门按 tgid 在队伍里找人,找到组长的身上记分,fork 出来的孩子 tgid 自立门户,爹的钟不跟过去,孩子要钟的话自己起。exec 换了戏服,任务还是原来的任务,钟是照走的。任务退了场,扫描见着死与僵尸都是跳过的,钟自然就哑了。一次进程立的是一本册,线程们共用的是它,这正是信号送给组的语义,ping 走的是单进程单线程,用着是正合身的。

到期的活挂在调度器的钟摆上,咱们跟着每一下滴答开头看一眼下界:下一次到点的时间是个原子量,现在还没到的话,滴答就直接走人了,队伍的锁都不碰,机器上的任务不多,这一眼就是钟摆的全部开销。下界到了才进扫描,扫描里最值得念的是周期钟的相位前移,原文是下面这一段的:

```cpp
if (scan->real_deadline_ns <= now_ns) {
    if (scan->real_interval_ns == 0) {
        scan->real_deadline_ns = 0;
    } else {
        const auto kTail       = (now_ns - scan->real_deadline_ns) % scan->real_interval_ns;
        const auto kDelay      = scan->real_interval_ns - kTail;
        scan->real_deadline_ns = kDelay > ~0ULL - now_ns ? 0 : now_ns + kDelay;
    }
    const auto kGate = scan->signals.gate.guard();
    if (scan->signals.actions[syscall::kSigAlrm - 1].handler != syscall::kSignalIgnore) {
        scan->signals.pending =
            scan->signals.pending |
            syscall::SigSet{1ULL << static_cast<unsigned>(syscall::kSigAlrm - 1)};
    }
}
```

咱们把扫描里的判决念一念。一次性的钟,到了期就清零,销了就完。周期性的,新到期不取整拍:错过的拍子拿间隔取余算尾巴,下一响落在现在加一个不满间隔的延迟上,落点是一定严格晚于现在的,不会一进了扫描又立刻到期,也不会把错过的每一拍补成一串信号,错过的拍子并成一声,这一声在信号门内记到待送的名册上。记之前还看一眼人家的家规:客人明写过 SIG_IGN 的这一声就不记,尊重是双向的。扫完了一遍,下一界的下界取所有钟的最小值,存回的是原子量,钟摆的快路径重新变便宜。

有一句如实的边界要挂在这儿:到期只记一笔,不走通用的送信。通用的送信会去叫醒睡死的任务,咱们这一卷的任务等在让位的圈里,一圈一圈地过门,门上是看得见名册的,记一笔就够了。下一卷的真睡上了场,记一笔是叫不醒睡人的,唤醒的路到那时再修。眼下咱们这套,ping 够用:它要的只是到点之后,下一圈让位回来时的那一眼。
