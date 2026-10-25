---
title: "04 · 发送与投递,两段路"
description: "信号走两段路:SendSignal 只管登记,过三关之后落在目标的待办集上,发送没有失败这个说法。DeliverPendingSignals 在 syscall 回程办,待办里挑没被屏蔽的号最小的办,一句回程最多送一个定制句柄。默认处置四种下场:带号死、蒸发、停走、照旧跑。RESSETHAND 的句柄送完一次整个回出厂。栈区间放不下投递的,按杀处理。投递的窗口只有回程,睡在等待表上的任务,信号要等它自己醒来才追得上。"
chapter: 21
order: 4
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - signals
  - scheduling
---

# 发送与投递,两段路

一个信号从生到死的全程,走的路是两段,咱们现在把两个名字立起来:发送占的是头一段,投递占的是第二段。发送干的是把号落在目标身上的活,投递是目标自己把事办了。为什么分两段?因为喊人的和办事的几乎永远不是同一个人:甲给乙发信号的那一刻,乙多半正在 CPU 上跑着别的活,或者压根就没排上 CPU 的份。咱们总不能让甲替乙把句柄跑了,所以甲只留记号,乙在自己的合适时机回头处理。这个合适的时机,咱们选在 syscall 的回程,上一篇那段分派代码里问的那一句,问的就是它。

咱们看任务身上添的家当。每个任务一座信号的小世界,装的是三样:一张二十二格的动作表,外加屏蔽的集合与待办的集合。全零就是合法的出厂态,句句的意思都是默认,什么都不欠。两张表的继承章程咱们也定了:fork 把动作表和屏蔽集整个抄给孩子,待办集是不抄的,孩子不背父亲没办完的事。exec 那头轮到咱们推倒重来,三样全回了出厂,新程序有新的章程。

发送的这一段路,正主说的就是 SendSignal,三道关全住在它的前半截里,咱们把这一截抄来:

```cpp
bool SendSignal(Task& target, int number) {
    if (number < 1 || number > kSignalCount) {
        return false;
    }
    if (target.state == TaskState::kZombie || target.state == TaskState::kDead) {
        return true;
    }
    const SigAction kAction = target.signals.actions[number - 1];
    if (kAction.handler == kSignalIgnore) {
        return true;
    }
```

头一关查的是号,出了合法区间的,答的是不行,这是唯一的拒绝,全函数唯一的 return false 就是它。第二关查生死:目标已经是僵尸或者死透了,咱们答照收,但什么都不做。死人是收不到信的,发信的人却没犯错,这句实话咱们得让发信的知道。第三关查蒸发:目标的动作表里,这个号装的是蒸发的话,咱们当场丢弃,答的还是照收,连待办集的门都不进。您留意三关的口径:除了号不合法,发送永远是成功的,信号世界的头一段路没有失败这个说法,而成败的裁判,归的是投递那一边。

过了三关,记号就该落在待办集上了,不过函数到这儿还没有写完,尾巴上的几行管的是停与醒在发送侧的活,咱们接着往下抄,您认个脸熟就行:

```cpp
    if (number == cinux::syscall::kSigCont) {
        target.signals.pending = target.signals.pending & ~stop_class_mask();
    }
    target.signals.pending = target.signals.pending | bit_for(number);
    if (target.state == TaskState::kStopped &&
        (number == cinux::syscall::kSigCont || number == cinux::syscall::kSigKill)) {
        Scheduler::self().resume(target);
        return true;
    }
    const bool kStopsNow =
        kAction.handler == kSignalDefault && DefaultDisposition(number) == SignalDisposition::kStop;
    if (target.state == TaskState::kReady && kStopsNow) {
        Scheduler::self().withdraw(target);
    }
    return true;
}
```

真正记事的只有置 pending 的那一行,裹在它前后的,全是停与醒在发送侧要办的差事:继续进了门,头一件事是把停类四个号的位清掉。落在停走的人头上的继续和杀,咱们当场就 resume,因为停走的人永远等不来自己的投递回程。落在排队的人头上的默认停,咱们当场就 withdraw,因为它的回程什么时候来,是没有保证的。这几笔在讲停与醒的那一篇里各有一场正戏,这里您知道它们住在谁家就行。

投递的这一段路,咱们跟着 DeliverPendingSignals 走。它只在 syscall 的回程被叫,咱们把正文抄来,进门的一段:

```cpp
bool DeliverPendingSignals(SyscallFrame& frame) {
    Task* const kSelf = Scheduler::self().current();
    if (kSelf == nullptr) {
        return false;
    }
    bool rewrote = false;
    for (;;) {
        const int kNumber = lowest_runnable(kSelf->signals);
        if (kNumber == 0) {
            break;
        }
        SigAction& action = kSelf->signals.actions[kNumber - 1];
        if (action.handler == kSignalIgnore) {
            kSelf->signals.pending = kSelf->signals.pending & ~bit_for(kNumber);
            continue;
        }
```

进门算头一笔的是 lowest_runnable 那一问:待办集抠掉屏蔽集,剩下的才是今天能办的,能办的里面挑号最小的办,低号的在前,这是 POSIX 立下的口味,咱们照办。蒸发路最省事:清了位接着 continue,循环转了回去挑下一个,耽误掉的工夫是一拍都没有的。然后咱们按动作词分路,分路的正身抄在下面:

```cpp
        if (action.handler == kSignalDefault) {
            if (DefaultDisposition(kNumber) == SignalDisposition::kTerminate) {
                die_by_signal(kNumber);
                break;
            }
            kSelf->signals.pending = kSelf->signals.pending & ~bit_for(kNumber);
            if (DefaultDisposition(kNumber) == SignalDisposition::kStop) {
                Scheduler::self().stop_current();
                break;
            }
            continue;
        }
        if (frame_handler_entry(frame, kNumber, action, kSelf->signals.blocked)) {
            kSelf->signals.blocked = kSelf->signals.blocked | action.mask;
            if ((action.flags & cinux::syscall::kSaResethand) != 0) {
                action = SigAction{};
            }
            kSelf->signals.pending = kSelf->signals.pending & ~bit_for(kNumber);
            rewrote                = true;
        } else {
            die_by_signal(kNumber);
        }
        break;
    }
    return rewrote;
}
```

默认路按处置表分岔:处置是杀的,咱们调 die_by_signal 让它当场带着号死,这一路是不回头的,死讯怎么编码,后面的篇专门讲。处置是停的,清了位之后把自己 stop_current,那也是后面专门的一篇。剩下的值是照旧跑,清了位接着走,十八号独一份:没停着的任务接到它,投递时清了位接着走,停走的任务,发送的那一拍就被它叫醒,停与醒的篇里细讲。定制路走的是 frame_handler_entry 那一问,它把帧改写到句柄的门口,这是下一篇的大戏。改写要是成不了,比如用户栈浅得摆不下投递要摆的东西,咱们按杀处理,栈区间的判定下一篇细看。还有一条纪律值得咱们单独说:一句回程最多送一个定制句柄,送完了就散,ring3 认的就是这个形状,堆着送会把用户栈摆成什么样,咱们谁心里都没数。纪律落到码里的位置,就是循环末尾的 break:咱们走完定制路,循环就散了,这是刻意安排的次序。装了 RESSETHAND 旗的,送完了这一次,action = SigAction{} 一格一格地清零,不只是把句柄词归了零,掩码旗子也一并归了位。

这里有一句诚实的边界,咱们把它画在明处:投递的窗口只有 syscall 的回程。一个睡在等待表上的任务,比如守着 wait 的父亲,信号落在它的待办集上,可它是不回程的,信号是追不上它的,要等它自己醒了、办完了事、走回程的那一步,信号才是追得上它的。您会问,那杀一个睡死的任务怎么办?咱们今天的答案是:等它睡醒。信号打断睡眠、把等待的 syscall 直接叫醒答 EINTR,这件事咱们和当年的工程线一样没有做,它需要给等待的链路再开一个口子,咱们留给真正需要它的日子。

宿主的长凳上,信号的套件这一批起了批,二十二案把两段路考了个全:表占了四案,发送占了两案,投递占了六案,找人的有一案,死讯配对的有一案,组的等待占了三案,停与醒占了五案。帧的校验案有个讲究:投递要往一个假想的用户栈上摆东西,咱们按内核自己的栈区间常量,在宿主里 mmap 出一块真的内存,把它摆在了那个位置上,引擎的地址算术跑的全是真常量,布局要是跑了偏,案子里当场红给您看。两段路是修通了,可定制句柄怎么进门、进门之后怎么回家,还是两张空白的纸,咱们下一篇把它们写满。

