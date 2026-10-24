---
title: "03 · 谁都能摇的泵"
description: "泵在这一章有了实在的形状:引擎叫 pump_once,身体一行,按预算 poll 一轮栈,预算十六帧的缰绳原样管着。线程 net_poll 只是调用者之一,TaskBuilder 起,调度权重二百,大数排队尾,合作式的低权重后台,身体是摇一次让一次的死循环,yield 不开中断也不停机。引擎与调用者分家,是旧案的正面兑现:生产线程摇,测试伸手摇,同一具引擎两个世界各自安全。两颗核上的分工也交代:排水的永远只有泵,发的人只递数据报,包序单线程的保障说的是发送人自己上下文里的事,计数是记分牌不当同步使。回环上跑顺,冒烟到提示符。"
chapter: 30
order: 3
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - networking
  - async
  - pump
---

# 谁都能摇的泵

泵这个词上一卷撂了一路,今天咱们给它一个实在的形状。NetWorld 的肚里有一具引擎,咱们叫它 pump_once,身体就一行:按预算把栈 poll 了一轮。十六帧的预算,是上一卷立的缰绳,原样地管着:一轮 poll 里,收环的排空,座上的派发,应答的记分,全都办了。引擎的全部就是这一行,咱们没让线程、中断与等待任何一样进去,谁叫它谁是一轮的老板。

线程是引擎的调用者之一,net_poll 是它的名字。咱们起它,用的是任务的那套词汇:入口填的是函数,名字填的是字符串,调度权重给了二百。权重的数越小越早被挑,二百这样的大数,排在队尾的意思,shell 与客人的活都赶在它前头,它是个合作式的低权重后台。线程的身体是一个死循环,一下一下地摇泵,一次一次地让位,永远地摇下去。让位用的是 yield:不开中断也不停机,这个世界的中断早在阶梯上就开了,泵是一声都不多管的,它的本分只有排水。

两段原文照树抄了来,引擎与坐泵咱们一起看:

```cpp
void NetWorld::pump_once() {
    stack_.poll(kPollBudgetFrames);
}

bool NetWorld::start_pump() {
    if (pump_running_) {
        return true;
    }
    auto* const kPump = proc::TaskBuilder{}
                            .set_entry(pump_entry)
                            .set_name("net_poll")
                            .set_priority(kPumpPriority)
                            .build();
    base::safety::Check(kPump != nullptr, "net pump task failed to build");
    proc::Scheduler::self().seat(*kPump, false);
    proc::Scheduler::self().unblock(*kPump);
    pump_running_ = true;
    return true;
}

void NetWorld::pump_entry() {
    for (;;) {
        self().pump_once();
        proc::Scheduler::self().yield();
    }
}
```

坐泵的那段里,咱们要多看一眼 seat 之后跟着的那笔 unblock,坐下的那一笔带的是不放行的实参,任务被置成挡着了,坐稳了 unblock 才放行,坐与行分了两拍。

引擎与调用者分了家,第一章的那桩旧案在这里正面兑现了。泵做成可注入的样子,生产线程摇的是它,测试的手伸过来也能摇它,同一具引擎在两个世界里是各自安全的。当年修法的笔记里,写的就是同一条路:等回包的人让位,排水的事归常驻的线程,谁也不在系统调用的栈上睡觉。咱们重走的这一遍,把它从当年的补救,立成了今天的结构,泵还没坐进机器的时候,pump_once 的引擎与亲手摇的测试就都已经写进了代码。

两颗核上的分工,咱们也交代清楚。排水的事永远只归泵一个,发的人只把数据报递进汇,构头封帧挂描述符的这一路全归汇,最后碰的是设备的寄存器与缓冲,碰不着泵的任何现场。所以包的次序是单线程的,两颗核也乱不了。这句保障管的是发送人自己上下文里的事,今天全机的发送人只有 kernel_init 一位,次序都记在它一个人的头上。第二位发送人进门的那天,发送的路要另立纪律,咱们把它登记在案。计数器当的是块谁都能看的记分牌,咱们不拿它当同步使,该让位的让位,该等窗的等窗,数据的一致由结构保证,分数不过是分数。

咱们头一步在回环上把异步世界跑顺。泵坐进 kernel_init 的开场,shell 是照常起的:泵与 shell 同住一台机器,冒烟一路跑到了提示符。回声请求发了出去,泵在后台把应答排了上来,记分牌动了,等待的人收工。这一套在回环上是零延迟的戏,纯内存的队列,时序攥在咱们自己手里,上一卷就是这么验的。真线的脾气比这大,大的地方有两处,设备等着咱们叫醒,而回包要人等。
