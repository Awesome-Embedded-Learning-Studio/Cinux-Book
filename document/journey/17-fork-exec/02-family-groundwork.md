---
title: "02 · 家族的地基"
description: "Task 立家族字段:pid 从 tid 改名、parent、child_link、exit_code,kZombie 入列。exit 分流:有父变僵尸定向唤醒,无父直通回收。wait_child 阻塞版睡到指定子退出,ECHILD 直拒。死者之家 detach:孙辈僵尸当场收,活孙改无父。祖母测试拿丢孩子 bug 是问,红路探针证明测试有牙,基线取样三犯把尺子放对了地方。"
chapter: 17
order: 2
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - process
  - scheduling
---

# 家族的地基

fork 之前咱们得有家。咱们从 Task 身上的字段起:调度器从出生那天起给每个任务发一个 tid,这一卷咱们给它改名 pid,再添三个词——parent 是谁可能等我退场,exit_code 是我退场时怀里揣的码,child_link 是我在全机孩子名册上的链结。状态表里咱们再添一档 kZombie。pid 和 tid 为什么合一?单线程的任务里,两个词在 Linux 那儿其实也是同一个数,咱们不为自己还没做的多线程预付词汇。

死了,不等于没了。exit 现在分了两条路,咱们把代码端出来:

```cpp
kPrev->exit_code = code;
detach_offspring(*kPrev);
if (kPrev->parent != nullptr) {
    kPrev->state = TaskState::kZombie;
    if (waiters_.remove(*kPrev->parent)) {
        unblock(*kPrev->parent);
    }
} else {
    kPrev->state = TaskState::kDead;
    reap_pending();
    pending_reap_ = kPrev;
}
```

有父的,变僵尸:尸身留在了世上,退出码揣在它的怀里,等的就是父进程来收。没父的,走进程那一卷直通回收的老路。为什么僵尸是必须的:父进程的 wait 是收据,子进程的退出码是货,咱们总得把货留到人家来开收据的那一刻。中间的那行唤醒,咱们写得省力——waiters_.remove 的返回值天然就是在不在表的判定,在表的,咱们就唤醒,不在的(父还没来等),什么都不用做,僵尸的尸身会一直留到父亲来。次序也交代给您:安置孩子、变完僵尸,然后咱们才唤醒父,生产排在消费的前头,唤醒到手的一定是成品。

没父的一路,还有半段的故事埋在那两行里,咱们说全。reap_pending() 收走的是上一任挂着的尸身,pending_reap_ 接住的是自己,这一收一挂的次序反不得——槽的容量只有一具,挂新的之前不收旧的,旧的那具就被顶成了无主,一桩真泄漏就是这么来的。至于为什么不能当场回收——垂死的任务这会儿还站在自己的内核栈上,reclaim 还的正是那四页栈,当场动了手,函数接下来的每一行都跑在已释放的内存上。收的活儿交给了切换,咱们看 park_current,wait_child 睡觉走的也正是它:

```cpp
void Scheduler::park_current() {
    Task* const kPrev = current_;
    kPrev->state      = TaskState::kBlocked;
    Task* next        = nullptr;
    if (ready_.pop(next)) {
        current_    = next;
        next->state = TaskState::kRunning;
        switch_tasks_(sink_, *kPrev, *next);
        reap_pending();
        return;
    }
    current_ = nullptr;
    return_to_main_(sink_, *kPrev);
}
```

switch_tasks_ 过后接着往下走的,是谁?是切换之后接着往下跑的那个任务,它踩的是自己的栈。这时候执行的 reap_pending(),把槽里那具尸身的栈页和 Task 对象还回去,才是安全的。您再看调用它的地方:rotate_from_interrupt 和 run_until_done 的尾巴上是同一行,三处守的都是同一句话:回收这活,永远在别人的栈上干。调度那一卷讲过退场的三个时刻——死亡、停止执行、可以释放,三处调用点守的,正是最后一个时刻落地的位置。

wait 的另一半是阻塞版。wait_child 拿一个想要的 pid,-1 表示的是随便哪个孩子。它的骨架是一个转不到头的循环,这回咱们把真身端出来:

```cpp
bool Scheduler::wait_child(long long wanted, ChildReceipt& receipt) {
    for (;;) {
        const cinux::arch::IrqGuard kGuard;
        Task* const                 kSelf = current_;
        cinux::base::safety::Check(kSelf != nullptr, "wait called outside any task");
        bool  has_any_child = false;
        Task* match         = take_zombie_child(*kSelf, wanted, has_any_child);
        if (match != nullptr) {
            receipt.pid  = match->pid;
            receipt.code = match->exit_code;
            reclaim_(sink_, *match);
            return true;
        }
        if (!has_any_child) {
            return false;
        }
        cinux::base::safety::Check(DefaultSpinLedger().depth() == 0,
                                   "task parked while holding a spinlock");
        waiters_.push_back(*kSelf);
        park_current();
    }
}
```

for 的每一圈,转的都是『醒了一遍』。咱们每圈开头落一把 IrqGuard 锁住中断,然后 take_zombie_child 扫全机的孩子名册,pop 出来的要是别人家的孩子,暂存的地方就是 keep 链,扫完了再原样放回。全机的名册只有一条,进出的动作都得倒这么一手。匹配的僵尸到手了,咱们把 pid 和退出码抄进取据,收尸的活儿交给 reclaim,用 return true 交了差。一个孩子都没有的话,咱们回绝 ECHILD,免得白白地睡死。手里还有没退场的孩子,咱们过一道自检,睡着的人手里不许攥着自旋锁。过了这一道,咱们把自己挂上等待表睡下,睡觉的地方,就是上面收尸的 park_current。醒来落在哪?还是 for 的头上,咱们再看一眼名册,还不匹配的话接着睡。伪唤醒是没有代价的:咱们要付的,不过是循环再转一遍。

还有一桩不好看的差事,总得有人干:死人的孩子怎么办。差事归了 detach_offspring,咱们私下叫它死者之家——垂死的任务把自己的孩子逐个安置:已经是僵尸的孙辈,没人会来收了,当场就回收了。还活着的,改成了无父,往后退场走直通回收的老路。不这么办,悬垂的 parent 指针就留在了世上——谁踩谁知道。

名册为什么安在调度器、不安在 Task 自己身上?这里有个 C++ 的硬限制,咱们如实交代——SelfList<Task> 要在 Task 的定义体里实例化,编译器还没见过完整的 Task,静态断言当场翻脸:链表类型做不了它所链类型的成员。所以孩子名册上移成全机一条,offspring_ 挂在了调度器手里,Task 带的只是链结。这个安排还白得一件好处:死者安置孩子的时候,咱们靠全机一条表就扫得到——不要求垂死者还能解引用自己。

syscall 面上添了三号,号位照 Linux 的原样:getpid 39、wait4 61、getppid 110,exit 60 早就进了册。号如今进了册,可 ring3 那头眼下一个能用它的客人都没有,shell 的 run 命令要等这一卷的末尾才接上,咱们把真正的实弹留在那儿交。

测试咱们多花点笔墨,因为这一批的测试真长着牙。头一桩是咱们自己逮自己的 bug:detach 的头一版,扫名册时把别人家的孩子 pop 出来,直接扔下不管——多家族同台的时候,静默丢掉别人家的孩子,连死者自己的僵尸都丢。修法是 keep 链回填:pop 出来不是自家的就暂存,扫完了放回去。修完咱们补了一案,咱们私下叫它祖母测试:两个家族同台,一家三代死绝了,另一家的 wait 必须照常收尸——一个孩子都没少。光绿其实还不够,咱们又做了红路探针:把病根那版代码跑真镜像,wait、zombie、祖母三处断言齐刷刷地翻红,等咱们把代码复原,绿也回来了。咱们让病根那版翻一回红,再让修好的版本回绿,这一红一绿都见过了,咱们才认它咬得住东西。

第二桩是尺子放错了地方。teardown 的测试要对页数,首版的基线取样取在了 init 之后,断言差了一页,当场红了。咱们让探针在三个时点各采了一个数,案就是这么定的:

| 时点 | 空闲页数 | 相对上一拍 |
| --- | --- | --- |
| 基线(首版,取在 init 之后) | 32317 | — |
| 测试布置完 | 32307 | 少 10,测试自己领走的 |
| teardown 拆完 | 32318 | 多 11,测试的 10 页连着根表那页一起还了 |

咱们从差值里读:进去的是 10 页,出来的是 11 页,多出来的那一页,正是根表的那页。它在基线取样之前就领走了,那笔 10 页的进出里没有它,拆的时候却照数归还。实现是对称的,错的是尺子:基线挪到了 init 之前,根表页计入领取的那一侧,三个数就成了首尾相抵的 32318、32307、32318,绿了。哦对了,还有一桩构建旧事同日翻出:默认构建不刷新机内测试的镜像,那一轮头一回跑出的 52 件全过,跑的其实是昨夜的旧内核,串口上查不到新测试的名字才穿了帮。咱们从此多了个做法:机内的绿,咱们要显式重建镜像之后再数,镜像目标也挂进了默认构建,谁也别想再悄悄地跑旧货。

AddressSpace 也添了 teardown:用户半区的整棵页表树,叶子、中间表、根逐层地归还。到了这一卷,一个任务退了场,它的世界跟着拆,帧也回了池子。下一节咱们站上这块地基,问 fork 那个要命的问题:复制什么,不复制什么。
