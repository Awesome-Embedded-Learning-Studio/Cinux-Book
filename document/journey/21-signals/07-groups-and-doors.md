---
title: "07 · 一伙人的门牌"
description: "Task 添 pgid 与 sid 两格,开机的骨干任务和 shell 各自成组自成会话,fork 抄父亲的。setpgid 只许动自己或同一会话的孩子,组要么是目标自己新立,要么是本会话已有的。setsid 已是组长的拒。kill 的号名字有四读:正数报号,零是本组,负一是除我全机,再小是组广播,广播扫全机名册。wait 学会组话,WNOHANG 有活子答零不停泊。用户面的包装同步到齐,sigdemo 住进盘上的 /bin。"
chapter: 21
order: 7
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - process
  - signals
---

# 一伙人的门牌

咱们手里信号这副嗓子,如今会的只是喊一个人的名字。可咱们开篇点的第三件缺,是喊一片的事:终端想喊的是一整伙后台作业,一条战线上的任务该一起收摊。Unix 的答案流传了半个世纪,它的名字叫进程组:一伙任务共挂一块门牌,喊门牌喊的就是全体。门牌的上面还有一层,叫的是会话,是一组门牌共有的院子。咱们把这层词添进 Task,两个字段:pgid 记的是门牌,sid 记的是院子。

出身咱们得定下来。开机的骨干任务,说的是从卷上拉起 shell、shell 退了场还负责重拉的 kernel_init,它和 shell 各自的 pid,就是各自的 pgid 和 sid,自成一个会话的头一份。fork 出来的孩子,抄的是父亲的门牌和院子,这是老家的常例,生在的是哪一户,就算的是哪一户的人。想换门牌的话,咱们走正门,五个号一起开了张,号位照的是 Linux:kill 占的是六十二,setpgid 占的是一百零九,setsid 占的是一百一十二,getpgid 占的是一百二十一,getsid 占的是一百二十四。

setpgid 的章程像过户,咱们把它的几道关卡摆开。咱们问的头一问,是动谁:动自己的话随时可以,动别人的话,那必须是我自己的孩子,而且孩子还没挪过院子,亲缘和会话的两头都得对上。咱们问的第二问,是进哪个组:目标自己的 pid,那是自立的新门牌,别的任何值,必须是本院子里已经立着的门牌。为什么必须有这一道?因为一块门牌的性命是跟着最后一个成员走的,组长死了组还在,承认一块谁也不挂的门牌,等于给发信号的人留了一张空头的名字。两问都有答的,咱们就改。回话的词分三档:名字找不着人的,答的是 ESRCH。人找着了不是自家孩子的,答的是 EPERM。人是自家的,门牌查无此组的,答的是 EINVAL。setsid 更简单:已经是组长的,咱们拒,答的还是 EPERM,不然的话就自立门户,自己的 pid 同时当 sid 和 pgid,答的是新 sid。

咱们再看 kill 这个号,它的名字有四种读法,咱们一张嘴一张嘴地说。正数的读法,报的是号,喊的就是那个人,这是咱们最熟的。零的读法是本组,喊的是包括我自己在内同门牌的全体。

负一的读法,是除我之外的全机,真到了那天,就是全体的起立。好在骨干任务是没爹没娘的,名册收的又是有父有母的任务,它坐进调度器的那一步就没上名册,死是天然免了的。再小的负数,读法是组的广播,喊的是门牌取负之后的那一组。还有个零号的信号:咱们只问人还在不在,东西是一个不发的,探针就是这么使的。

四种读法落进的是同一个 HandleKill,咱们把正文抄来:

```cpp
long long HandleKill(const SyscallFrame& frame) {
    const int        kNumber   = static_cast<int>(frame.rsi);
    const auto       kNamedPid = static_cast<long long>(frame.rdi);
    proc::Scheduler& scheduler = proc::Scheduler::self();
    if (kNamedPid > 0) {
        proc::Task* const kTarget =
            scheduler.find_by_pid(static_cast<unsigned long long>(kNamedPid));
        if (kTarget == nullptr) {
            return -kEsrch;
        }
        if (kNumber == 0) {
            return 0;
        }
        if (!proc::SendSignal(*kTarget, kNumber)) {
            return -kEinval;
        }
        return 0;
    }
    // Zero names the caller's own group, -1 every other task on the
    // roll (the bring-up init has no parent, so it is not on the roll
    // to begin with), and anything below names the group at -pid.
    if (kNumber != 0 && !number_served(kNumber)) {
        return -kEinval;
    }
    proc::Task* const        kSelf  = CurrentTask();
    const bool               kAlone = kNamedPid == -1;
    const unsigned long long kGroup =
        kNamedPid == 0 ? kSelf->pgid : static_cast<unsigned long long>(-kNamedPid);
    unsigned long long heard = 0;
    scheduler.for_each_task([&](proc::Task& scan) {
        const bool kReach = kAlone ? &scan != kSelf : scan.pgid == kGroup;
        if (kReach && (kNumber == 0 || proc::SendSignal(scan, kNumber))) {
            ++heard;
        }
    });
    if (heard == 0) {
        return -kEsrch;
    }
    return 0;
}
```

头一支只认的是正数,走的是喊一个人的老路,零号在这里现出探针的原形:人找着了、号又是零,咱们答零走人,连 SendSignal 的门都没进。后头的三读合走一条,折算的活全在 kReach 那一行:零和组号被折算成了门牌,负一折算出来的却是另一把尺子,量的是除我之外的全机。真正承重的是括号里 kNumber == 0 那半句:号是零的那一回压根不会调 SendSignal,人却照样记进了 heard,探针只问不发的章程,落到码里的就是它。广播怎么落地?咱们给调度器开了一条走全机名册的路,for_each_task 持着中断守卫地把名册从头扫到尾,单核的机器上,回收和扫描在同一个守卫下串行地走,扫的人只读门牌和发信号,半路收尸的事谁也不干。一个都没喊着的,数下来的 heard 就是零,咱们答 ESRCH,喊人没喊着是不算错的,但话咱们总得有个交代。

wait 这边也学会了听门牌。从前收据上的 pid 只有两读:报一个具体的号,或者负一挑的是随便谁。现在的读法有四种:负一的照旧,零挑的是本组的孩子里随便谁,再小的负数挑的是那一组的孩子,正数的还是报具体的号。与它一起来的还有一个新旗,WNOHANG:有匹配的孩子活着,可还没一个退场的,咱们答零,人是不会停泊的。这把钥匙开的是轮询的门,一个想看着孩子又不想睡死的父亲,从此有了正路的问法。wait 的第三个参数,从前被丢弃的那个,今天有了真的语义。它就住在 rdx 的最低一位上,咱们把那几行抄来:

```cpp
    constexpr unsigned long long kWNoHang = 1;
    const bool                   kNoHang  = (frame.rdx & kWNoHang) != 0;
    proc::ChildReceipt           receipt{};
    if (!proc::Scheduler::self().wait_child(kWanted, kNoHang, receipt)) {
```

wait_child 收进去的,一头是四读挑人的号,一头是不停泊的许诺,里头怎么挑怎么答,是生与收那一卷讲过的老家什,今天咱们只是让它多认了一面旗。

门牌立起来了,有一桩边界咱们照实说:孤儿不送人收养。当年的工程线把没爹的孩子挂到 init 名下,咱们的死者之家走的是另一条路,孩子就地散伙,僵尸是当场收的,活着的摘了链自己走。为什么不同?因为咱们的收养人还不存在,名册上也不许留悬垂的指针。会话和门牌的后半本,控制终端、后台读写终端的那几个号什么时候真的开火,要等终端进门的那些卷,咱们把词备下了。

用户的那一头,咱们把包装一次配齐:Kill、Setpgid、Setsid、Getpgid、Getsid、Sigaction、Sigprocmask,外加带 WNOHANG 的 wait 包装。四参的包装里,第四参落的是 r10 的座,那是懒到家那一卷实弹炸出来的章程,如今写进了公共的家什。还有一个新住户住进了盘上:一个走全链的演示程序,它的戏码是 fork 出孩子、立门牌、探针、停、醒、组杀,整条链从头演到了尾,验收那一篇的主角就是它。喊一个人的门都开了,可三件缺里最难的还是停与醒:喊停了,它凭什么真的停。下一篇咱们把这件事做实。

