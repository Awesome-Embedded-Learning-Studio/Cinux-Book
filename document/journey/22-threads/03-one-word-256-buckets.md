---
title: "03 · 一个词,二百五十六只桶"
description: "几条流在同一个词上碰头:值还是这个值你就睡,值变了有人叫你。二百零二号开门,两个操作码,超时一味没抓。二百五十六只桶只管收窄搜索,桶里的人揣着精确地址,同桶不同词不误醒。查值与进桶在同一把锁里办完,唤醒掀的是同一把锁,经典的丢唤醒窗口从结构上关死。首版唤醒只醒一个,QEMU 当场挂死,改成按数循环才稳。"
chapter: 22
order: 3
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - futex
  - sync
---

# 一个词,二百五十六只桶

基址立好了,咱们来办第二件缺的:词上的等待。您想一个最日常的画面:两条流共用一个计数,一条流等着计数归了零再去收摊。最笨的等法是自旋,一个循环里没完地读,读到了就走。可自旋是白烧 CPU 的等法,单核的机器上它还有更坏的脾气:等着的人占着 CPU 转圈,能改这个词的人反而上不了 CPU,您等的就是您自己拦下的。所以等待要交给内核管:这个词还是这个值,你就把 CPU 交出来睡,有人把词改了,他来叫你。这套握手有个流传了二十多年的名字,叫的是 futex,咱们照 Linux 的号给它开门:二百零二。

它的脸面很小,咱们把门口的三道检查说了。等的和叫的都递一个词的地址过来,地址四字节对齐,歪了答 EINVAL。地址得在用户半区里,越了界答 EFAULT。然后分的是两个操作码:WAIT 是零,带的第三个参数是期望值:内核在锁里读那个词,值还是期望值的话就泊车,值已经变了,当场答的是 EAGAIN,号码十一咱们是认得的,它报的是提醒:您来晚了,词已经动了,您要不要再看一眼,是您自己的事。WAKE 的号是一,带的是叫醒几个人,答的是实际叫醒了几个。别的操作码咱们不认,答的是 ENOSYS。超时这一味咱们没抓,当年的工程线也没抓,咱们如实交代,挂在欠着的那页上。

门里头是本节的主角,咱们从桶说起。全机的等待者收在一张表里,表分成了二百五十六只桶,词落的是哪只桶,算的是地址往右挪两位再按二百五十六取余,咱们把这两行抄来:

```cpp
    [[nodiscard]] static unsigned long long bucket_for(unsigned long long uaddr) {
        return (uaddr >> 2ULL) & (kBucketCount - 1ULL);
    }
```

挪两位的道理是词是四字节一户:取余定桶。您一眼就能看出来,不同的词会落进同一只桶,哈希碰撞是整张表的家常便饭。所以桶只管一件事:把搜索收窄。真正认人的,是等待者自己揣着的那个精确地址,Task 上新添的 futex_uaddr 一格。唤醒的人进了桶,逐个比对揣着的地址,一个字符都不差的才叫醒,同桶不同词的,睡人家的觉,一个误醒的也没有。二百五十六只桶挂的是同一把闸,管整张表的是一把自旋锁。为什么不分桶加锁?因为单核一台机,两把桶锁当然不可能同时被人拿着,分了也是白分,而锁的名册咱们是当真记的,懒到家那一卷立的章程,进名册的锁都得报得出自己的名字,合并成了一把,咱们在名册上也只记一个名字。

等待者挂桶用的是链,Task 身上又添了一条链结。咱们把链数一遍:互斥锁、信号量、完成件,同步的三件套,咱们让它们仨共用一条。家长孩子的名册占一条。调度器的等待表占的是任务身上那枚继承来的 SelfNode 老底座。如今词上等待又开了一条,咱们数下来,这已经是第四条了。一链一节点的家法,咱们交过两回学费,生与收那一卷的双链案,懒到家那一卷的等待案,两案的教训是同一句:一枚节点配一个 next,两条链挤的是同一枚节点,入链的那一拍就抹断了另一条。所以 futex 自开一条:谁也不挤谁。整套握手您看着眼熟就对了,轮流当家那一卷里互斥锁与信号量的等法,就是它的原型:自旋锁护的是表,链结挂的是人,泊车走的是 block_current,唤醒走的是 unblock,咱们看下来路数一字不差,变的只有一样,人泊的地方从内核自己的信号量,换成了用户世界里的一个词。

要紧的学问在丢唤醒上,咱们慢慢说。经典的丢唤醒是这样发生的:等的人查了词,值对上了,打算睡了,可叫的人赶在他挂上队列以前跑了一遍唤醒,队列里没有他的名字,白叫了,他这才安心睡下了,从此没人再叫了——这一觉睡到天荒。防它的办法只有一种形状:查词的那一拍和进桶的那一拍,必须是原子的,跟唤醒看见的那一拍,锁的是同一把。咱们的实现就是它,等待侧的真码咱们整段抄来:

```cpp
base::KernelError WaitOnUserWord(unsigned long long uaddr, unsigned int expected) {
    Task* const kSelf = Scheduler::self().current();
    FutexTable& table = shared_table();
    {
        const cinux::arch::IrqGuard kQuiet;
        const SpinGuard             kGuard(table.gate);
        auto const* const           kWord = reinterpret_cast<volatile const unsigned int*>(uaddr);
        if (*kWord != expected) {
            return base::KernelError::kWouldBlock;
        }
        kSelf->futex_uaddr = uaddr;
        table.buckets[FutexTable::bucket_for(uaddr)].push_back(*kSelf);
    }
    Scheduler::self().block_current();
    return base::KernelError::kOk;
}
```

咱们看这段代码,原子窗口就是那对花括号:咱们进门就关了中断,再把表闸也拿了下来,读词、记地址、进桶的三件事全住在括号里,括号收拢的那一拍,锁就已经放了。承重的有两行。头一行是锁内的读词:值要是变了,返回的就是 kWouldBlock,系统调用那一层把它翻成的正是 EAGAIN,您来晚了的那句话,说的就是它。push_back 那一行把自己挂进了桶里,挂上了,叫的人才看得见。泊车的 block_current 特意放在括号外头:他睡下的时候锁早已还了,可桶里已经有他的名字。唤醒侧掀的是同一把闸:

```cpp
long long WakeUserWord(unsigned long long uaddr, unsigned long long count) {
    FutexTable&                 table = shared_table();
    long long                   woken = 0;
    const cinux::arch::IrqGuard kQuiet;
    const SpinGuard             kGuard(table.gate);
    auto&                       bucket = table.buckets[FutexTable::bucket_for(uaddr)];
    Task*                       node   = bucket.head();
    while (node != nullptr && std::cmp_less(woken, count)) {
        Task* const kNext = bucket.after(*node);
        if (node->futex_uaddr == uaddr) {
            bucket.remove(*node);
            Scheduler::self().unblock(*node);
            woken++;
        }
        node = kNext;
    }
    return woken;
}
```

唤醒的人进闸以后就沿着链走,谁揣的地址与词不差一个字符,咱们才摘他的链、唤醒他、给他计数,数够了收工。所以叫的人要么赶在等的人进桶以前跑完,那时候等的人还没决定睡,词上的值已经变了,他锁内一读就知道了,要么就在等的人进桶之后,那他是一定看得见的。等的和叫的谁也错不过谁,这个窗口在结构上就没了。进桶的那一拍在前,泊车的那一拍在后,两拍的次序就在上面两段代码里摆着。

本节咱们还交一枚真案的学费。首版的唤醒人手里只有一面停泊旗,叫了一声,旗翻了一下,收了工。可词上等的可能是两个人,叫的人明明白白递了二,他只醒了一个,另一个还在桶里睡得好好的,而能叫醒他的那一声已经喊完了。咱们拿实弹一跑,QEMU 当场挂死了,挂得一点声音也没给咱们,机器安安静静地停在没有人会醒的地方。咱们的修法是把唤醒改成清点:进桶沿链走,地址对上的摘链、唤醒、计数,数够了才收工,一面旗的懒办法退场。您看,丢唤醒的凶处就在它的安静里,它没有崩溃的响动,没有断言的字样,它只是让一部分人永远地睡下去,所以它逼着咱们把窗口的结构想透,想透了还不够,还得数着人头一笔笔地清点。

词上能等人了,几条流碰头的章程立了。可还有一个问题咱们没办:几条流要共用一个世界,那共用的家当,总得有地方放、有法子分。下一节咱们搬家,搬的是打开的文件。

