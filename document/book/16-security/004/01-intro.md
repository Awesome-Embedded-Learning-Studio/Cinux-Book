---
title: 01 · 导引:全局 stac 在 SMP 下会丢
---

# 导引:全局 stac 在 SMP 下会丢

> 上一章(056)开 SMAP 的办法,是在所有从用户态进来的入口各挂一对 `stac`/`clac`——`syscall_entry` 一进来就 `stac`,三个中断宏在「从用户态进」的分支里也 `stac`。等于一进内核就把 RFLAGS.AC 拉高,整段内核态都放行用户内存访问。单核下这套没毛病。可一旦上了 SMP,它就变成一颗定时炸弹:**RFLAGS.AC 是 per-CPU 位,而 context_switch 切任务时根本不存 RFLAGS**。任务从 CPU0 迁到 CPU1,新核上的 AC 是它自己的旧值(多半是 0),代码却还在按「全局 stac 已开」的旧假设裸解引用用户指针——撞上 AC=0 的核,SMAP #PF。`-smp 2` 下 shell 反复 `/hello`,`sys_waitpid` 写用户 `*status` 必崩。
>
> 这一章把那颗炸弹拆了,顺手还掉上一章末尾埋的债。两件事:其一,**撤掉入口级的全局 stac**,改成局部 `stac`/`clac` 的 user accessor——只在真正拷贝用户内存的那一小段窗口里放行 AC,拷完立刻关;所有「内核直接解引用用户指针」的路径迁到 accessor 上,syscall 按Linux 的样子切成 `do_*_kernel`(纯内核逻辑,可以 block)/ `sys_*`(薄边界,只管跨用户)两层。其二,**给 accessor 配上 exception table**——拷贝中途真 fault 了(用户传了个不可映射的地址),靠一张 RIP-based 的 `__ex_table` 把执行改到 fixup,accessor 返回 false,syscall 返回 `-EFAULT`,而不是 panic。这正是 056 章最后那句伏笔:「完整的 `copy_from_user`/`copy_to_user`(带 exception table 的容错访问)是后面的事」。
>
> 验证不靠「用户可见的新能力」,靠两件可观测的事——accessor fault 的负测试(解引用未映射地址,返回 false 而不是把内核炸了)、exception table 纯函数的 host 单测,加上全量测试在单核和 `-smp 2` 下都不回归。
>
> 一条诚实的边界先说在前头:**本机是 WSL2,不透传 SMAP**,所以「SMAP 真拦了一次内核访问用户页」在本机看不到。但这一章要验证的两件事都不依赖 SMAP 真生效——accessor fault 的负测试走的是内核态 accessor 指令的 fault(由 exception table 拦,跟 SMAP 开没开无关),分层和窗口纪律更是纯代码正确性。SMAP 真生效要换真机或 TCG;accessor 做对,跟环境无关。

## 这章咱们要点亮什么

1. **为什么「入口级全局 stac」天生跟 SMP 过不去**:两个事实叠一起就够——RFLAGS.AC 是 per-CPU 位,而 context_switch 的 `CpuContext` 里压根没有 RFLAGS 这一格。这不是某个边角 case,是模型本身跟 SMP 冲突。
2. **局部 stac/clac 的 accessor 取代全局 stac**:`access_ok` 先把坏范围挡掉,真正拷贝就是一扇 `stac` → 拷 → `clac` 的小窗,这扇窗**绝不阻塞、绝不调度**——否则又是 AC 跨核丢失的重演。
3. **syscall 切两层,铁律才有落点**:把「可以 block、可以摧毁地址空间、可以持内核状态」的内核逻辑(`do_*_kernel`)和「碰用户内存」(`sys_*`)彻底切开,「block 的时候不持用户指针」这条纪律才落得下来。
4. **exception table:accessor fault 的容错契约**:一张启动时排好序的 `__ex_table`,每条记录「这条 accessor 指令 fault 了就跳到这个 fixup」;PF handler 在最前面查它,命中就改 `frame->rip`,accessor 返回 false,syscall 返回 `-EFAULT`。这是 Linux `copy_from_user` 的契约,替代了之前「靠 demand-page 兜底、真不行就 panic」的赌法。

## 上一章埋的雷:全局 stac 在 SMP 下会丢

先把上一章怎么开的 SMAP 回顾一下,雷就埋在那套挂法里。

056 开 SMAP 时,`stac` 挂在两个地方。一是 SYSCALL 入口,因为 SYSCALL 必从用户态进来,handler 一定要读用户内存,所以一进来就放行:

```asm
swapgs
stac                # set AC: handler may touch user memory
```

(`syscall.S:66`。`stac` 原挂在该 `swapgs` 下一行,现已注释,见 :67。)二是中断入口,三个 ISR 宏各挂一对,而且只在「这个中断是从用户态进来的」分支里 `stac`(跟 `swapgs` 同条件),内核态中断继承当前 AC——这点上一章解释过,是为了不破坏嵌套里正在进行的 `copy_from_user`。

单核下这套为什么没毛病?因为只有一个 CPU。AC 在那个核上 `stac` 设上了,就一直在,直到某个出口 `clac`。任务切来切去都在同一个核,RFLAGS 是同一份。

**SMP 下这套假设就塌了。** 问题不在哪一行代码,在两个物理事实:

**事实一:RFLAGS.AC 是 per-CPU 位。** 每个核有自己的 RFLAGS,AC 是其中的 bit 17。CPU0 上 `stac` 设的是 CPU0 的 AC,CPU1 的 AC 它管不着。

**事实二:context_switch 切任务时不存 RFLAGS。** 看 `CpuContext` 这个结构——它就是 context_switch 存取任务现场的那块内存,布局写在 `context_switch.S` 头注释里:

```
Offset 0:  r15
Offset 8:  r14
Offset 16: r13
Offset 24: r12
Offset 32: rbp
Offset 40: rbx
Offset 48: rsp
Offset 56: rip
Offset 64: gs_base      (RESERVED — GS is per-CPU)
Offset 72: kgs_base     (RESERVED — KERNEL_GS_BASE is per-CPU)
Offset 80: fs_base      (per-thread TLS)
```

(`context_switch.S:9`。)这里头存了 callee-saved 通用寄存器、栈指针、返回地址、还有 per-thread 的 FS base。**没有 RFLAGS。** 函数头也直说了:`Clobbers: flags`(`context_switch.S:39`)——它压根就不保存标志寄存器。

两个事实一叠,炸弹就响了。想象任务 A 在 CPU0 上跑 `sys_waitpid`,走到了「写用户 `*status`」这一步——上一章的全局 stac 假设此刻 AC=1,代码直接 `*status = exit_code` 裸写用户内存。偏偏这时钟中断来了,A 被切走,调度器把它扔到 CPU1 上接着跑。CPU1 的 RFLAGS.AC 是多少?是 CPU1 自己的值——它从没替 A 执行过 `stac`,AC 大概率是 0。可代码还在按「全局 stac 已开」的旧假设继续裸写 `*status`。AC=0 + SMAP 开着 + 写用户页 = SMAP #PF。

实证很干脆:`-smp 2` 下 shell 反复 `/hello`,跑着跑着 `sys_waitpid` 这一写就撞上 AC=0 的核,页故障。查 pte 不是页表坏了(不是 0 就是合法映射),是 AC 丢了。

> 根因一句话:**「全局 stac」这个模型跟 SMP 在物理上不兼容**。它假设「一进内核 AC 就全程放行」,可 AC 是 per-CPU 的,根本没有「全局」可言。只要任务会跨核迁移,「入口处设的 AC」和「实际执行时的 AC」就可能不是同一份。修它不是打补丁,是把模型换掉。
