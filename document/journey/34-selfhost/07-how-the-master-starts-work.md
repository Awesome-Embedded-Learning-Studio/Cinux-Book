---
title: "07 · 师傅怎么开工:spawn 与保留旧世界"
description: "点名单上有一号,咱们答的明明是不办,师傅却每轮开工都要敲它一遍:GCC 16 的班里,领班每叫一位匠人,用的都是 posix_spawn,它背后的 glibc 起手探的就是 clone3,听见 ENOSYS 便退回 clone,递上共世界与 vfork 两面旗,这声不办是两边约好的暗号。vfork 的旗在线程那一卷的清单之外,如今入列:孩子带着父亲的世界跑,父亲挂起,完成件架在这次调用的内核栈上,等 exec 成功或者孩子退场那一声才恢复。失败的 spawn 一样照合约走:exec 失败不毁旧世界,glibc 写回 errno,孩子活着退场,waitid 按 Linux 的 siginfo 布局开收据。futex 那边上了一课:libstdc++ 与 glibc 的词带着私有位来,128、129 当新号答 ENOSYS,glibc 核对错误码不依,当场 abort 报 unexpected error code,私有位原来是旗,摘了旗照旧是 0 与 1。长凳添两件:真实编译轮,缺了 wrapper 的 spawn 失败探针。开工的路通了,交接收活的时候,笔者一条改令下来,exec 的参数整个按正本重写。"
chapter: 34
order: 7
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - selfhost
  - syscall
  - gcc
---

# 师傅怎么开工:spawn 与保留旧世界

点名名单上有一号,咱们答的明明是不办,师傅却每轮开工都要敲它,敲完也不恼,转头把活干完。那个号是 clone3。咱们请进门的是 GCC 16,它的领班每开一道工序都要叫起一位匠人,预处理的、编译的、汇编的、链接的,一位一道。叫人的家什统一是 posix_spawn,落到系统调用上,glibc 起手探的就是 clone3,一扇 Linux 后来才另开的新门。探一声,听答话。咱们答的是 ENOSYS,glibc 心里有数了:新门没开,退回来,走老的。

所以明说不办那一栏里,clone3 坐得理直气壮。glibc 的路数两头都写明了:clone3 答 ENOSYS,就退到 clone。咱们这一头要是装聋,让这个号没人应答,答案虽然也是 ENOSYS,整轮验收却要按 unserved 记败。有人应答,答的是不办,暗号才立得住。回退的路,两边是提前约好的,谁也不欠谁。

退回来的 clone 递上两面旗。一面是共世界,CLONE_VM,线程那一卷的清单上它排头一个:孩子的地址空间直接指父亲的,爷俩跑在同一个世界里。另一面是 vfork,线程那一卷立清单的时候,它没写进去。清单之外的旗,咱们整个拒掉,答 EINVAL,半份的忙不帮。照旧章程办,glibc 这一手要当场吃闭门羹。

两面旗都得收,收 vfork 还得给它真的语义。咱们把处境摆出来:孩子带着父亲整个的世界跑,栈和堆都是两家共用的家当。这段日子里,父亲凭什么敢撒手睡去?

敢。glibc 那头的合约把另一头也写明白了:父亲睡去的日子里,孩子只办两件事,exec 成功,或者退场,办完其中一件,父亲才醒。Linux 给 vfork 定的就是这个语义。咱们拿线程那一卷的手艺接活:fork 的全套照走,共世界的旗把孩子的世界指回父亲的,再把一件完成件挂到孩子身上。接活的几行,您贴着看:

```cpp
    proc::Completion done;
    if ((kFlags & kCloneVfork) != 0) {
        kChild->vfork_done = &done;
    }
    proc::Scheduler::self().unblock(*kChild);
    if ((kFlags & kCloneVfork) != 0) {
        done.wait();
    }
    done.retire();
    return kPid;
```

完成件是线程那一卷传下来的老件:一次性的状态,finish 把它从等待挪到终态,只挪一次,wait 把当前任务泊到终态为止。咱们再看它安在哪儿:父进程这次系统调用的内核栈上,孩子身上带的是指针。父亲一进 clone 就睡在 wait 里,孩子放进队列,爷俩的接力全看完成件那一声。

> 咱们没拿现成的信号量顶这个差,有讲究:多出来的一枚令牌会放跑下一个等的人。一次性的状态到了终态就封笔,多喊一声也改不了什么。

放行的那一声,两个地方都能喊。咱们看第一处,exec 的末尾:参数铺完,新的世界装好,连返回的帧都改写了,末尾才轮到它。

```cpp
    if (kSelf->vfork_done != nullptr) {
        auto* const kDone = kSelf->vfork_done;
        kSelf->vfork_done = nullptr;
        kDone->finish(true);
    }
    return 0;
```

另一处是退场:孩子要是不 exec、直接退出,收尾的代码同样把完成件喊完,父亲照样醒。咱们把两个口子一合计,父进程恢复的时机就一个不差:孩子要么换好了新世界,要么已经退了场,共世界的那段日子,父亲全程不在场。

合约的失败路径,一行一行也得兑现。咱们故意让叫人这件事本身失败:GCC 的领班有个 -wrapper 选项,让它把每位匠人经由一个中转程序去叫。咱们递过去一个不存在的路径 /absent,叫人的头一步就得出错。出错那一刻,孩子的世界一草一木没动:exec 办不成就原路退回,glibc 的孩子把 errno 写回共用的内存,然后自己退场。父亲醒来读到错,gcc 报给敲命令的人,退出码交一。

失败的孩子也得有人收。glibc 收 spawn 的孩子用的是 waitid,点名名单的三种答法里,它站在真办那一栏。收据的四个格子,点名名单那一章咱们已经一格一格数过,信号号、来路、孩子的号、交代本身,版式照 Linux 的 siginfo 布局落,glibc 拿到的与它在 Linux 上用惯的一模一样。这一回的不同只在开收据的人,spawn 失败的孩子不 exec 别的世界,把 errno 写回共用的内存就退场,waitid 收的就是孩子退场时留下的交代。生与收那一卷立过老话:wait 是收据,退出码是货。wait4 的收据是一格 int 的暗码,waitid 的收据宽到一百二十八字节,一格格写明白。

门上安静了,futex 那边又上了一课,这课是 glibc 亲手教的。libstdc++ 与 glibc 的锁,词上等待,操作码递来的却是 128、129。线程那一卷立的章程还在:操作码只认 0 与 1,别的不认,答 ENOSYS。咱们照章办事,glibc 当场翻脸:它对错误码较真,认得的错才走得下去,陌生的 ENOSYS 不在册,直接 abort,报的是 unexpected error code。

翻案翻得干脆。把 128 与 129 摊开看:号还是 0 与 1,各自加了一面私有旗。旗说的事很老实:这个词只在本进程里用,等待的队列不必跨进程共用。桶表咱们全机只用一张,本来就没想到按进程分家,把旗摘掉再认号,语义一点不缺:

```cpp
    constexpr Word kPrivate = 128;
    const Word     kOp      = frame.rsi & ~kPrivate;
```

摘旗的两行就位,两家的锁都走得通了。这一课咱们记在案上:号位上来了新面孔,得辨一辨,它是新号,还是老号添了旗。

长凳上跟着添了两件。一件是真实编译轮:as、ld、gcc、g++ 的全链在串口壳里真跑一遍,纪律也一并立下,轮里出现任何一声 unserved,整轮记败,编译器的每一步都不许靠装聋混过去。另一件就是缺了 wrapper 的探针,咱们在自家盘上敲下去:

```text
gcc -wrapper /absent /hello.c -o /bad; echo SPAWN_FAIL_$?
```

咱们要的判据只有一行:`SPAWN_FAIL_1`。退出码交一,这一分记在失败路径头上。

开工的路通了。真到交接收活的时候,笔者下了一条改令:exec 把参数交给新程序的那一套,整个按正本重写。正本是 Linux 内核源码里的 fs/exec.c。下一节,咱们照着正本把交接重办一遍。
