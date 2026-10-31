---
title: "10 · 一号座位上的真当家人"
description: "开卷排的次序走到一号座:从前一号座上是内核自家的监护线程,壳是它直拉的孩子,如今用户态的 init 预占一号,装载与 exec 共用同一套,前几章给 exec 交的学费它直接继承。监护人退到二号守在旁边,只保位子不当家,当家的倒了它按原样再拉一个。孤儿不再断线,过户给一号座上的当家人,生与收那一卷立的收尸家法,从这里交到当家的手里。配套号位一串:termios2 的四个新词进 ioctl 词书,getpgrp、getgroups、setgroups 开门,glibc 的 sleep 落在 clock_nanosleep 上,clone 的 CHILD_SETTID 让孩子号记在孩子自己名下。实弹第一响是旧案:当家的起手往 reboot 门递关近路的字条,旧门不查一个数就断电,修后门里立检查,开机串口报出 Cinux up。开机这出戏戏本换人,当家的照两行当班表开台,respawn 兜底,/proc/1/stat 里一号座坐的是真身。getty 登录戏这一卷不上,当班表是最小形状,后面的卷还要添行。当家的坐镇了,家里该添设施——头一件是搬进搬出。"
chapter: 33
order: 10
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - process
  - init
  - pid1
  - syscall
  - reboot
---

# 一号座位上的真当家人

开机的座次,这一章翻了个面。号位件在上一节配齐了,可一号座上还是内核自家的监护线程,壳是它直拉的孩子,当家的始终没露面。上一卷收卷撂下的四件方向里有一件就是它,一号进程的 init,这一章兑现。为什么一号非它不可,客人自己把话挑明了:busybox 的 init 开场头一件事是查自家的号,查到不是一号,当场撂挑子退出,报错那句话原样照录,must be run as PID 1。一号不预留给它,它连台都不上,咱们这一章就整章白写。

坐法得动内核的座位表,咱们把发号的老路翻出来看:next_pid_ 递增,谁排头谁拿小号,内核线程起得早,一号天然归它。如今的坐法是给 seat() 多递一个 initial_process 位:

```cpp
    if (initial_process) {
        for (const Task* scan = tasks_.head(); scan != nullptr; scan = tasks_.after(*scan)) {
            base::safety::Check(scan->pid != 1, "initial process already seated");
        }
        task.pid = 1;
        if (next_pid_ == 1) {
            ++next_pid_;
        }
    } else {
        task.pid = next_pid_++;
    }
```

您对着头几行看:扫一遍座位表,确认一号还没人坐,防线是那句断言,原句 initial process already seated。然后 pid 不走递增,直接写成 1,发号器顺带跨过一号,后面的人从二号起拿。次序照代码排:监护人 kernel_init 的身子造好,拿它当爹,造出用户 init 的任务入座领一号,封锁不让跑,kernel_init 随后入座领二号。等它跑起来,把 /sbin/init 的整个世界装进那个预备好的身子,装完才放行一号。开机打印把这场交接报了出来,老新两行摆在一起:

```
[kern] boot task (pid 0) parks here, kernel_init (pid 1) takes over
[kern] boot task parks here, kernel_init prepares user init (pid 1)
```

头一行是旧报法,一号给了 kernel_init。第二行是新的,kernel_init 只是备座,一号留给用户 init。装世界的家什您该眼熟,build_shell_world 走的正是 exec 那一套,ELF 装进空间,解释器照 PT_INTERP 去请,情报桌往栈上摆。前几章给 exec 交的学费,auxv 添的两味,栈照 PT_GNU_STACK 自报户口,当家的上座全数继承。kernel_init 自己的差事没换,守的循环照旧:一号座上的人哪天倒了,wait_child 收到音讯,按原样再拉一个。打印的措辞还是老词,说的是 shell 退场,respawning,词是旧词,事换了人,咱们如实照录。保位子的是监护人,不当家,当家的永远是一号座上的用户态 init。

当家的上座,家里另一桩家法跟着换手。旧法是生与收那一卷立的,咱们还认得:爹走在孩子前头,孩子断线成孤儿,僵尸由内核代收。如今 find_adopter 接手:

```cpp
Task* Scheduler::find_adopter(const Task& dying) {
    for (Task* task = tasks_.head(); task != nullptr; task = tasks_.after(*task)) {
        if (task != &dying && task->pid == 1 && task->space.holds() &&
            task->state != TaskState::kZombie && task->state != TaskState::kDead) {
            return task;
        }
    }
    return nullptr;
}
```

条件咱们念一遍:坐在一号座上,人活着,手里托的是真用户空间,僵尸与死透的不算。都过,孤儿过户给当家人,连已成僵尸的一并过户,过户时给新爹递一笔 SIGCHLD,把打盹的他叫醒。收尸的家法从内核手里交到当家的手里,交得不冤:busybox init 的主循环本来就靠 wait 一个一个收孩子,收完翻当班表,谁的记号是 respawn,谁就该重拉。真找不到这样的当家人,旧法顶上,内核代收。

号位那边,当家的上座前后又递进来一串单子,咱们照单补座。termios2 的四个新词,TCGETS2、TCSETS2、TCSETSW2、TCSETSF2,进了 ioctl 的词书:TCGETS2 取终端的参数,TCSETS2 设,TCSETSW2 等积压的输出排空了才设,TCSETSF2 排空之外连没读的输入也冲净。当家的开台要照新词跟终端对表。getpgrp 问自家小组的号,getgroups 与 setgroups 管身份的花名册,一座一座开。clock_nanosleep 是二百三十号,glibc 的 sleep 就落在它上面,这座不补,客人睡不成。clone 的两面旗分开记:CHILD_SETTID 是这一卷的新章法,旗子一带,孩子号记在孩子自己名下。CHILD_CLEARTID 是线程那一卷的旧例,孩子退场时把号清零,再摇铃叫醒等它的人。glibc 的线程收摊,两面旗一起递。单子厚了不奇怪,伺候的从直拉的壳换成了当家人,礼数跟着涨。

实弹的第一响,响在机器刚点火的时候。当家的坐上一号座,查完自家的号,跟着就往 reboot 门里递一张字条,这是 busybox init 起手的做派。字条上的命令数是 RB_DISABLE_CAD,数值为零,意思是把 Ctrl+Alt+Del 硬重启的近路关掉,源码里这行旁边躺着一句注释,原词是 misnomer,名不副实,他们自己也这么嫌。旧 reboot 门里没有一道检查,进门就摸电源,魔数不查,命令数不查,递字条的人是谁也不查。机器刚开机,屏幕黑了,当家的连台词都没来得及说。翻案翻在门里,咱们把头一段摆出来:

```cpp
long long HandleReboot(const SyscallFrame& frame) {
    if (CurrentTask()->euid != 0) {
        return -kEperm;
    }
    if (frame.rdi != kRebootMagic1 || frame.rsi != kRebootMagic2) {
        return -kEinval;
    }
    if (frame.rdx == 0) {
        return 0;
    }
    if (frame.rdx != kRebootPowerOff) {
        return -kEinval;
    }
```

从上往下念。递字条的不是 root,EPERM 拒。两位魔数对不上,EINVAL 拒,0xFEE1DEAD 与 672274793 是 Linux 定下的老暗号,咱们一字不改地认。命令数为零,答零走人,零正是关近路的意思,事情办了,busybox 那边连回话都不看。命令数既不是零,也不是 0x4321FEDC 这个真关机的数,EINVAL 拒。修完开机,串口里当班表头一行的动静报出来了,就一句,Cinux up。

开机这出戏,戏本也换了人写。从前的戏本里没有当家人,内核直拉一个壳,提示符是内核亲手递到您眼前的。如今 /sbin/init 坐镇,照 /etc/inittab 里的当班表开台,表眼下两行,上一章咱们整张念过,这里不再重搬。您开机自己摸一遍证:

```
/ # cat /proc/1/stat
1 (init) R 2 1 1 0 -1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
```

您把回话挨个字段看:头一个数是一,括号里名字是 init,状态 R 跑着,爹的号是二,小组与会话也都是一号。一号座上是用户态的真身,爹是监护人,ps 里的全家福排得明明白白。

当家的自己怎么值夜,咱们看它的活法。它的等待靠两样家什,一样是 rt_sigtimedwait,想守着信号睡,另一样是 wait,一遍遍问有没有孩子退场。咱们门里,头一样是口假铃,恒答 EAGAIN,从不真睡。

> rt_sigtimedwait 恒答 EAGAIN 是第一遍就判下的,防的是值夜的客人真睡进等待里,再没人叫得醒。这一遍没有翻案,照旧。

铃是假的,当家的睡不实,咱们看着它的圈:sleep1 让它每秒醒一回,问一遍 wait。带 respawn 记号的行退了场,它记一笔,源码里那笔的原词是 Scheduling for restart.,下一圈把行重拉,一秒上下,提示符回来了,开台的戏不停。

真正的登录戏,getty 问口令那一路,这一卷不上,当家的眼下只管开台与兜底。当班表也是最小形状,后头的卷还要往上添行,添什么,轮到那些卷咱们再排。当家的坐镇了,家里该添设施——头一件是搬进搬出。
