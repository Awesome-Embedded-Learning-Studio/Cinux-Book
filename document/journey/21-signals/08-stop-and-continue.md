---
title: "08 · 停得下来,醒得过来"
description: "状态名册从五档长到六档,新添的叫 stopped。看得见的内核那一卷记的两笔在这里交割:类的队列立了摘人的纯虚一问,兑现。跨类的次序这一卷没动,如实登记。调度器添三件:stop_current 抱着内核栈换人,resume 把停走的放回队,withdraw 把排队的就地摘。动作分两个时机:发送时办醒人与就地摘,投递时办自己停走。停走的任务醒在调度器的恢复点上,syscall 照常把话说完。"
chapter: 21
order: 8
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - scheduler
  - signals
---

# 停得下来,醒得过来

三件缺里的末一件,也是最难的一件。咱们喊停,任务凭什么真的停?停不是死的下场,死了尸身留在世上等收。也不是睡的下场,睡了还挂在等待表上等叫。停是第五种活法:人是不动了,可接下去的动作,咱们随时都接得上。咱们得给状态名册添一档。从前的五档,咱们念过:就绪、在跑、阻塞、僵尸、死透。现在第六档在中间安了家,它的名字叫 stopped,咱们叫它停走档。panic 报状态的名册同步补了名字,编译器开着漏一档就拒绝编译的开关,这一档想悄悄地不上名册,都是不行的。

可光有状态是不够的。一个任务停了走,它的身影就得从所有能被选中的地方消失,而能被选中的地方,如今握在调度类的手里。这就是看得见的内核那一卷里,讲调度类那篇末尾记下的两笔,今天咱们当面交割。头一笔是类的队列要学会摘人,咱们兑现了:SchedulingClass 在四问之外添了一手纯虚的 remove,把一个排在自己队里的任务摘出去。为什么归了类管?因为队列是类自家的家当,人排在第几位、抽走之后后面怎么补位,知道的只有类自己,这跟 pick 是它家的差事,讲的是同一个道理。纯虚的一问在声明里就一行:

```cpp
    virtual bool remove(Task& task) = 0;
```

您回头看两个落户的类交的实现:定容的数组逐格地找到人,抽走了一格,后面的往前补上。

调度器的这一边,咱们配上三件兵器,三件的正文都在调度器这里,咱们按件的次序抄。头一件的名字叫 stop_current,管的是自己停:

```cpp
void Scheduler::stop_current() {
    const cinux::arch::IrqGuard kGuard;
    Task* const                 kSelf = current_;
    cinux::base::safety::Check(kSelf != nullptr, "stop called outside any task");
    cinux::base::safety::Check(DefaultSpinLedger().depth() == 0,
                               "task stopped while holding a spinlock");
    kSelf->state = TaskState::kStopped;
    Task* next   = pick_next();
    if (next != nullptr) {
        current_     = next;
        next->state  = TaskState::kRunning;
        slice_ticks_ = 0;
        switch_tasks_(sink_, *kSelf, *next);
        reap_pending();
        return;
    }
    current_ = nullptr;
    return_to_main_(sink_, *kSelf);
}
```

它把状态改成了停走,照泊车的路数把 CPU 交给下一个人,可咱们什么都不挂,任何等待表上都不留它的链。进门那两道 Check 是护命的,后一道问的是自旋的深度:停走的人是要抱着内核栈睡的,攥在手里的自旋锁要是没放,等锁的人就等成了没有头,所以深度不是零的话,这一关是过不去的。pick_next 选好了下家,switch_tasks_ 跳了出去,函数就停在了半路,要把它叫醒的人没来,后面的 reap_pending 和 return 就轮不到。第二件的名字叫 resume,管的是唤醒:

```cpp
void Scheduler::resume(Task& task) {
    const cinux::arch::IrqGuard kGuard;
    cinux::base::safety::Check(task.state == TaskState::kStopped,
                               "resume of a task that is not stopped");
    task.state = TaskState::kReady;
    cinux::base::safety::Check(task.sched_class->enqueue(task), "ready queue full at resume");
}
```

resume 的正文咱们看过了:查的状态确实是停走,改回去的是就绪,送进的是类的队列,三行就是它的全部。第三件的名字叫 withdraw,管的是就地摘:

```cpp
bool Scheduler::withdraw(Task& task) {
    const cinux::arch::IrqGuard kGuard;
    if (task.state != TaskState::kReady) {
        return false;
    }
    const bool kQueued = task.sched_class->remove(task);
    cinux::base::safety::Check(kQueued, "ready queue lost a task the stop signal named");
    task.state = TaskState::kStopped;
    return true;
}
```

只对还在就绪队里排着的生效,咱们让类把人摘出来,查到队列里丢了人的,咱们响亮地拒绝。您再留意一笔:摘完人,顺手把状态也改成了停走,发送侧的 withdraw 和投递侧的 stop_current,到的都是同一个档。

真正要紧的分野在时机:同一件停的事,发送的时候办一半,投递的时候办另一半。咱们按信号落在谁的头上来分,分的是三种情形。落在排队的人头上:它还没上 CPU,自己的投递回程什么时候来,是没有保证的,所以默认停类的信号在发送的那一拍就 withdraw,人就地摘出队列。落在跑着的人头上:它自己的回程就在眼前,信号进的是待办,回程投递的时候,咱们清了位,自己把 stop_current 调了下去,收了工,syscall 把该答的话照常答完。落在已经停走的人头上,咱们单独说。

停走的人有个要命的性质:它是永不被调度的,永远回不了自己的投递回程。所以给停走的人发继续,咱们不能等它自己投递,得在发送的那一拍由发信人亲手来办:当场 resume,把人放回了队列。发给停走的人杀,咱们同样当场 resume,为什么?因为它得醒来走上自己的死路,死讯的编码才进得了退出词,父亲的 wait 才收得到。别的信号给停走的人,落的是待办,它醒了之后回程再办。继续这一号还有个前置的动作,咱们也摆出来:发送时咱们把十九号到二十二号这停类的四个号清得干干净净,再置上自己的位。不清的话,醒来转脸又吃一记停的招呼,继续也就不成其为继续了。

投递侧的停是这样的。任务在自己的回程上清了位,之后调了 stop_current,把 CPU 交了出去,从此整个内核的 C 栈抱在怀里,停在切换的正中间。继续来了,resume 把它放回了队列,轮到它上 CPU 的那天,切换器把照片装了回去,那一跳落在轮流当家那一卷讲过的恢复点上,对 C++ 的世界来说,stop_current 像的是一次普通的函数调用,调了进去,过了很久,正常地返回了。返回到哪?返回到投递的循环里,循环收了尾,分派把话说完了,syscall 的答案原样地出了它的栈。咱们看整个过程,被停的那笔 syscall 没有被打断半个字,这就是停走和睡眠、和死都不一样的地方。

测试是在两面开花的。宿主的五案:排队的挨停,当场就出了队,继续把停走的放回了队,继续把待办里停类的四个号清掉,杀能唤醒停走的而普通的终止请求不能,投递侧的案子还会自己把自己给停走。机内两案走的是真上下文,这是宿主的长凳够不着的:一案让任务自己停走,咱们断言它留在栈上等唤醒,名册是清空的,唤醒之后旗子才立了起来。另一案里咱们让兄弟俩同台,一个停了走,另一个在中途把它抬了起来,访问的计数恰好是一。ktest 的调度套件从二十二案长到了二十四案。

然后咱们交割第二笔,原原本本地说:跨类的次序,这一卷是没有动的。问询还是注册的次序,头一个注册的类不清空,后面的类没人问过。停走把人从类里摘走了,可次序的裁定没有跟着动,今天的咱们只学会了摘人,排位是没有学会的。真需要两类任务互相裁定谁前谁后的日子来了,咱们再把次序的裁定重新裁过。名册上记着的就是这句,咱们不装做办过了。

停与醒立住了,卷首立的三件缺算是齐了。收卷以前咱们还有一段插曲,是验收的时候插桩替咱们抓出来的一条死链,它跟信号的本事无关,跟测试的家底有关。下一篇咱们看灯下的死链,然后咱们再收卷。

