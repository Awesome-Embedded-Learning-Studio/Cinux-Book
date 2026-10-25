---
title: 05 · 决策的不管动手
description: "Scheduler 只管语义:队列、状态、挑人、回收时机,真动手的切换是 SwitchSink 的四个动词,类型擦除注入。host 长凳装个录决策流的假 sink 就能逐条断言,首跑就抓到真 bug:两个任务接连退出,单槽待回收把退得早的覆盖泄漏。死亡、停止执行、可以释放,是三个时刻。"
chapter: 12
order: 5
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - scheduling
  - testing
---

# 决策的不管动手

债也还了,任务也会造了,现在回答蹦床一节欠下的第四道题:照片拍好了交给谁。答案是一座新盖的房子,`kernel/proc/` 里的 Scheduler。咱们把它的人设说定:调度器的全部价值在决策——谁上、谁等,谁该退出、什么时候回收。而切换这个动作本身是压栈、存寄存器、换指针,纯粹的机器活。这两件事咱们故意不让同一个类干,劈成了决策的半边和动手的半边。这么劈的直接收益是可测性:决策可以被断言,而动手只能被实弹,长凳上测不了的从来是动手那半边,咱们干脆把它抽走,剩下的决策半边整个搬到 host 上验。这个手法您见过两次了:走查降成 TableWorld 之上的模板,心跳把芯片擦成了函数指针加上下文,已经是第三回了,用的还是同一门手艺。

动手半边的脸,咱们给它立成一个 concept,列出来的就是四个动词:

```cpp
template <typename Sink>
concept SwitchSink = requires(Sink& sink, Task& outgoing, Task& incoming) {
    { sink.switch_tasks(outgoing, incoming) } -> std::same_as<void>;
    { sink.enter_from_main(incoming) } -> std::same_as<void>;
    { sink.return_to_main(outgoing) } -> std::same_as<void>;
    { sink.reclaim(outgoing) } -> std::same_as<void>;
};
```

四个动词念一遍您就有底:任务换任务(switch_tasks)、主流程进任务(enter_from_main),跟着的是任务回主流程(return_to_main),末了的是给死任务收尸(reclaim)。Scheduler 对外只认的也是这四句话,咱们换谁来实现它都认。内核里装的是真家伙,咱们叫它 KernelSwitchSink,四个动词全数落到 ContextSwitch 的身上,外加一副主流程自己的照片:主流程不是任务,可它也得有地方存照片,不然任务切回来时找不到回主流程的路。长凳上装的是假货,一个 RecordingSink:每个动词干的是两件事,把发生了什么记进日志,顺手把新任务的函数当场调起来往前推。长凳上没有真的切换可做,代办的法子是一次嵌套调用:run_until_done 走进头一个任务,任务里喊了 yield,假 sink 就在 yield 的深处调起下一个任务,一层套一层地叠下去,跟套娃似的。整条执行链就这么嵌着跑,退栈的时候层层退回主流程,最后那笔 reclaim 记录才跟着落地。调度器每做一次决策的当口,决策流都被逐条记了下来,断言念的就是日志原文——覆盖泄漏首跑就被按在长凳上,乒乓一节那六步序列的形状推得出来,靠的正是这套嵌套代办。这哪里是模拟?跑的代码和内核里跑的是同一份 scheduler.cpp,假的只是最后那一步切换。类型擦除的注入用的是心跳那套原样:lambda 转函数指针,上下文的指针跟着走。

决策半边的家当,咱们不再干念——这就把码打开。底子咱们摆两句:一条 RingQueue 当就绪队列,存的是 Task 指针,容量十六——自己的脸、自己的耳朵那一卷立进 base 的轮子,在这儿正好是它的原生形态:轮转调度干的就是弹出队头再塞回队尾。当前的活儿由 current_ 一个字段记着,它不驻队:被选中的任务从队里出来,让出的时候才回队尾。这个形状是跟 Linux 学的,好处是队列里永远只有等着的人,没有正在跑的人。入座的入口是 seat,它的真身一共五行:

```cpp
void Scheduler::seat(Task& task) {
    const cinux::arch::IrqGuard kGuard;
    cinux::base::safety::Check(sink_ready(), "scheduler has no sink installed");
    task.tid   = next_tid_++;
    task.state = TaskState::kReady;
    cinux::base::safety::Check(ready_.push(&task), "ready queue full at seat");
}
```

这五行干的是三件差事。您头一眼看到的是发号:`task.tid = next_tid_++` 把 tid 从 1 起一路往后发,名字是 TaskBuilder 早填好的,状态在这里也置成了就绪。第二件是 push 的返回值喂给了 Check,座位满了就当场叫停。这里咱们把话说重一点:入队失败要是无声无息地过去,丢的就是一次唤醒——一个任务从此永远消失,不崩溃、不报错、毫无异样地排在日志里。丢了唤醒的 bug 比死机难查十倍,这道口咱们就交给 Check 把着。第三件是进门的那把 IrqGuard,它是关中断的守卫,队列也是共享财产的道理,讲到锁家族的时候才正式开讲,这里咱们认个脸就够。

然后是本节的正菜:一个任务退场的三个时刻。头一个时刻叫死亡:入口函数返回了,退场钩子叫醒了 exit_current,任务的状态标成 kDead。跟着的是停止执行:切换发生,它的照片存档,CPU 从此跑别人的代码。末一个是可以释放:它的栈、它的 Task 对象被还回去。为什么是三个时刻而不是一个?您想想死亡那一刻的处境就明白了:死亡那一刻,任务还踩在自己的栈上,exit_current 这行代码就跑在它的栈里——偏偏这一刻,要是当场 delete 掉了 Task、把栈还给名册,等于锯自己坐着的那根树枝。所以回收必须等切换完成,在新任务的栈上、在调度器的临界区外面办。落实的形状是一个 pending_reap_ 槽:退场时把死者挂上,等下一次切换落地,收割的活才由新任务代劳。

出事的就是这个单槽,首跑就出了岔子。长凳的假 sink 第一趟跑下来,断言报出来的事是:两个任务接连退出,退得早的那个没有被回收,它的 reclaim 一次也没来。咱们把这一版的 exit_current 摆出来,对着码看它的病根:

```cpp
void Scheduler::exit_current() {
    const cinux::arch::IrqGuard kGuard;
    Task* const                 kPrev = current_;
    cinux::base::safety::Check(kPrev != nullptr, "exit called outside any task");
    kPrev->state = TaskState::kDead;
    reap_pending();
    pending_reap_ = kPrev;
    Task* next    = nullptr;
    if (ready_.pop(next)) {
        current_    = next;
        next->state = TaskState::kRunning;
        switch_tasks_(sink_, *kPrev, *next);
    } else {
        current_ = nullptr;
        return_to_main_(sink_, *kPrev);
    }
}
```

它把任务标成了死,把死者挂上了 pending_reap_ 槽,然后从队里挑了下一个、发起切换,队空了就把 CPU 还给主流程——三个时刻,一段里就全齐了。承重的是标死和挂槽之间的那一行 reap_pending()。当年的写法没有它,病根朴素得让人脸红:甲退出的瞬间,pending_reap_ 指向了甲。还没等咱们切过去,乙也退出了,pending_reap_ 被改成了乙,甲就这么被覆盖丢了,栈和对象全成了无主的孤儿。修法跟病因一样朴素:挂新的之前,把旧的收掉。收旧的那半边是这么一段:

```cpp
void Scheduler::reap_pending() {
    Task* const kDead = pending_reap_;
    if (kDead == nullptr) {
        return;
    }
    pending_reap_ = nullptr;
    reclaim_(sink_, *kDead);
}
```

槽空着的时候就原样退回,槽里有人的时候才喊 reclaim。这个动词正是 SwitchSink 四个里的末一个,它的差事,是把死任务的栈和对象还回去。喊它的时机全在切换落地之后:exit_current 的尾巴上、run_until_done 的每一轮后面,跑这行代码的都是别人,锯树枝的困境就这么绕开了。一行次序的调整,换来一个真泄漏的现形,host 假 sink 的首跑就把它按在了长凳上——这一抓,正是决策/动手分离在替咱们站岗:要是这段逻辑只能在机器上验,这个泄漏得等名册的页数哪天对不上数才会露头。

咱们再翻一件旧案:当年的 exit_current 还犯过另一桩错,改 current_ 在前、发起切换在后的写法,等到发起切换去读 from 的时候,current_ 里坐的已经是新任务,from 和 to 成了同一个人。这样的切换等于白拍一张照片、又原样装了回去——任务既没死、也没回队,退出来的时候还是它自己,下一拍又发起一模一样的切换。咱们从外面看到的就是机器在原地打转。这一版的写法从根上堵死了旧病,您看真身的头两行:kPrev 在 current_ 换人之前就落进了本地变量,切换的 from 从它身上读,读到的永远是发起切换前的旧人——发起切换的人手里握的是落袋的 prev,不是那个随时会被改写的 current_,旧案的病根就这样被写法本身堵死了。

末了把 spawn 的分家交代了。真到要造一个任务再让它入座的时候,内核侧的路数是三步:TaskBuilder 把任务造了出来,Check 又把它验了一遍,seat 把它送进了座位。咱们没有把它合成一个便利函数。理由嘛,咱们把它摆在参数上:排序权重一来、调度类一来,合起来的接口就得拆。咱们让座与造分着立,各自长各自的,分家留的缝,咱们留着。

决策的半边立住了,动手的半边真枪实弹装好了。两个任务真打一场乒乓的戏,咱们这就开演。
