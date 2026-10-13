---
title: "07 · 两种等待"
description: "任务在,睡 Completion:一次性判决 Pending 到 Success 或 Failed,判完唤醒队头,重开判决时谁也不许在睡。开机时没有任务,睡 EnableIrqAndHalt:sti 和 hlt 挨着写,睡眠才是原子。两条禁令说在前头:别持着自旋锁睡,别把这一对展开成两笔;醒来之后,条件必须重查。"
chapter: 15
order: 7
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - storage
  - synchronization
---

# 两种等待

盘的回话走中断,判决落在哪儿?咱们给判决找了个落点。这一卷往同步的家当里添了个新原语,叫 `Completion`,一次性的判决书:

```cpp
bool finish(bool success);
bool wait();
[[nodiscard]] bool finished() const;
[[nodiscard]] bool success() const;
void rearm();
```

面孔您多半觉得眼熟——进程那一卷里,信号量干的就是一边睡、一边被人叫醒的营生。`Completion` 是同一个家族的孩子,脾气更绝:它一辈子只宣判一回。出生时是 `Pending`,`finish` 一到,落成 `Success` 或者 `Failed`,从此改口不改判——`finish` 第二回来,原话奉还,判决不动。宣判的那一下,还要替睡觉的人办事:

```cpp
state_            = success ? State::kSuccess : State::kFailed;
Task* const kNext = waiters_.pop_head();
if (kNext != nullptr) {
    Scheduler::self().unblock(*kNext);
}
```

判决落定的同一拍,`finish` 把等待队列的队头摘下来、叫醒。睡的人在 `wait` 里把自己挂进队列,醒来再读一遍判决,该干嘛干嘛。这就接上了咱们卷过的老话:等的人睡觉,干活的人叫醒,叫醒的活儿判决书自己包了。还剩一个 `rearm`——判决书用完一回,下一道命令还得用,翻回 `Pending` 就是了。翻页有条件,咱们看它开头那一行:

```cpp
cinux::base::safety::Check(waiters_.empty(), "rearm with a waiter still parked");
```

还有睡客没醒,翻页当场炸。想想也对:人家睡得正香,等的正是自己的判决,您把判决抽走重写,人家醒来读到的就是别人的案子。所以翻页之前,咱们总得看清睡客都走了没有。

这套手艺伺候的是任务。可开机那会儿,调度器还没开工,咱们手里只有主流程一条执行流,`wait` 想睡也没处睡。另一条路在引擎那头等着:

```cpp
bool Ahci::wait_for_completion() {
    if (proc::Scheduler::self().current() != nullptr) {
        return completion_.wait();
    }
    for (;;) {
        const unsigned long long kSnapshot = cinux::arch::SaveAndDisableIrq();
        if (completion_.finished()) {
            cinux::arch::RestoreIrq(kSnapshot);
            return completion_.success();
        }
        cinux::arch::EnableIrqAndHalt();
    }
}
```

一个分岔,两条路。有任务,咱们睡 `Completion`。没任务,进那个死循环——关中断查一回判决,没到,就 `EnableIrqAndHalt` 睡到下一回中断,醒来从头再查。循环看着笨,其实每一步都有讲究,尤其睡觉那一下。`EnableIrqAndHalt` 的实现,一共两条指令:

```cpp
void EnableIrqAndHalt() {
    __asm__ volatile("sti; hlt" : : : "memory");
}
```

开中断,然后睡。咱们把这两步看仔细:为什么必须挨着写,分成两笔就不行?这里有个硬件的善意,STI 指令一执行完,处理器不马上收中断,还憋着一拍——这一拍叫 STI shadow。`sti; hlt` 挨着写,开中断之后的头一件事就是睡觉,于是“醒”与“睡”之间不存在缝:中断来了,处理器从 `hlt` 的下一拍接着跑,睡就是原子的一步。要是写成 `sti` 一句、`hlt` 另一句,两句话之间那道缝里,中断就能钻进来——处理函数跑完了,判决发完了,然后 `hlt` 才执行。睡觉的人这一觉就白睡了:要等的事已经过去,他得挨到下一回不相干的中断——多半是时钟——把自己拍醒,重查一遍条件,才拿回本来就落袋的判决。

两条禁令,咱们替您写在明处。头一条:持着自旋锁的时候,别调它。您想想那个场面——锁在手里,人睡了,别人来拿锁,转圈,能放人的人,也得拿这把锁,也转圈。一条锁,拴住一串人。第二条:别自作聪明把它展开成“恢复中断,再睡一觉”两笔,刚才说的那道缝就是这么来的。这两笔,头一遍搓内核的时候都真踩过,脚印还在注释里。

还有半句,也写在注释里:醒来必须重查条件。能把 `hlt` 拍醒的,不止您要等的中断——时钟每 10 毫秒响一回,谁的线开了它都拍得醒。所以循环长那个样子:查、没到、睡、再查。醒来就当判决到手的人,拿到的常常是时钟的问候。

两条路合在一个分岔里,等判决的手艺就齐了。下一节,咱们把盘包装成所有任务都能问的门面,再把排队的章程立起来。
