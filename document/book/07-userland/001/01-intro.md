---
title: 01 · 跳进 Ring 3:用户态与特权隔离
---

# 跳进 Ring 3:用户态与特权隔离

> 上一章(021)我们给内核补上了同步原语,多条执行流终于能在锁和信号量的护送下有序地共处了。可有一面墙始终没立起来:内核和「用户态」之间的那道墙。从 010 章点亮大内核 GDT 起,我们就在 GDT 里留好了 Ring 3 的代码段(`0x1B`)和数据段(`0x23`),却一次都没真正切进去过——整本内核从头到尾跑在 CPL 0。这一章就把墙立起来:用 `SYSRET` 一脚跨进 Ring 3,再让用户代码里的第一条 `cli` 撞墙弹回来,触发一个 `#GP`。串口吐出的 `CS=0x001b` 和那行 `protection works!`,就是特权隔离成立的证据。要诚实地把范围说在前面:这只是「跳进去、撞墙、停住」的一次性演示——用户态还没法跟内核说话,没有 syscall、没有 ELF 程序、也没有调度回环,这些是下一站的事。

## 这一章我们要点亮什么

核心是让内核第一次真正进入 **Ring 3**,并被**特权隔离**拦住。拆开看交付五块。

第一块是 SYSRET/SYSCALL 的 MSR 装配。全新的 [usermode.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/usermode.S) 里,`usermode_init_asm` 写三只 MSR:`STAR(0xC0000081)`、`SFMASK(0xC0000084)`、`EFER(0xC0000080)`,把 `EFER.SCE` 位打开,这是 SYSRET/SYSCALL 能用的前提。`jump_to_usermode(entry,user_stack,arg)` 按 SYSRET 的寄存器契约摆好现场,一条 `sysretq` 跨进 Ring 3。

第二块是用户态段 + TSS 的落地。GDT 里早就留了用户代码/数据段,这一章真正补上的是给 TSS 配一块独立的 4 KB Double Fault 栈(`df_stack_[]`,`alignas 16`),`tss_.ist[0]` 指向它的栈顶;再提供静态方法 `GDT::tss_set_rsp0(rsp0)` 写 `tss_.rsp[0]`——这是 Ring 3 一旦触发异常、CPU 自动切回内核栈时的着陆点。

第三块是 IDT 给 `#DF` 挂上 IST1。路由表从 4 元组升级成 5 元组,多一个 `ist` 字段;Double Fault(vector 8)挂 `ist=1`,配合上面 TSS 的 `ist[0]`,烂栈上再炸一次也不会把内核拖死。

第四块是用户地址空间 + 用户页。`launch_first_user()` 新建一个 `AddressSpace`,在 `0x400000` 映一页代码(塞进四字节字节流 `cli;hlt;jmp .-2`),在 `0x7FFFFF000` 下面映射 4 页栈,全部带 `FLAG_USER`。配套的关键改造在 `vmm.cpp`:`walk_level` 加了 `user_flag` 参数,把 `FLAG_USER` 从 `VMM::map` 一路传到 PDPT/PD/PT 每一级。

第五块是 `#GP` 区分来源。`handle_gp` 用 `from_user = (frame->cs & 0x03) != 0` 判断异常来自内核还是 Ring 3,来自 Ring 3 的 `#GP` 多打一句 `protection works!`——这就是 milestone 的验收信号。

合起来,这一章证明了「Ring 3 真的进得去,而且真的被特权层挡住」。但要划清边界:用户态此刻没法和内核通信——它只能触发异常,然后内核 `cli;hlt` 停机。`launch_first_user()` 不会返回,`main.cpp` 里它后面的键盘轮询循环在本 demo 里**不可达**(后面调试现场会点破为什么头注释里那句「Scheduler init」是没擦干净的遗留)。这是一个「单向往返」的演示,不是完整的用户态。

## 为什么现在需要它

为什么非要用 `SYSRET` 进 Ring 3,而不是看起来更熟悉的 `IRETQ`?因为 `IRETQ` 是中断返回,它要做一整套动作:读 IDT 查段权限、按 `SS:RSP` 压栈/出栈、校验栈段选择子的 DPL。每一步都是「为了从中断里安全出来」而存在的保护。可我们现在不是从中断里出来,我们是**主动、干净地**把一个全新执行流送进 Ring 3——没有要恢复的中断现场,没有要校验的栈帧。`SYSRET` 正是为这种场景设计的快路径:它不查段描述符权限、不压栈、不读 IDT,只做三件事——`RCX→RIP`、`R11→RFLAGS`、按 `STAR[63:48]` 推出 Ring 3 的 CS/SS。少做事就是少出错,这就是我们选它的理由。代价是它对寄存器有硬性契约(下一节展开),摆错一位就 `#GP` 或更糟。

为什么特权隔离要靠两层,缺一不可?第一层是**页表的 user 位**。x86-64 四级页表里,只有当一条虚拟地址在 PML4、PDPT、PD、PT **每一级**的条目都带着 `FLAG_USER`(bit 2)时,Ring 3 才允许访问它;任何一级缺这个位,CPL 3 的访问就被拒掉,`#PF` 错误码里 `U/S=1`。这意味着「用户能访问哪些内存」是由页表逐级决定的——内核可以高枕无忧地把内核代码、内核栈所在的页全部不带 user 位,用户态连读都读不到。第二层是**特权指令**。`cli`、`hlt`、`wrmsr`、`mov cr*` 这一类指令被 Intel 列为 privileged instructions,CPL 不是 0 就直接 `#GP`。这两层一起才叫「隔离」:页表决定用户能看到什么内存,特权指令决定用户能做什么动作。少任何一层都不算隔离——只有页表没特权指令,用户能 `cli` 关中断把整个系统搅乱;只有特权指令没页表,用户能随便读写内核内存。

那为什么 `SFMASK` 在这一章里写了也基本等于白写?`SFMASK`(`IA32_FMASK`)这只 MSR 只对 **SYSCALL** 方向生效:它决定 SYSCALL 进来时 `RFLAGS` 会被清掉哪些位。而 `SYSRET` 完全不读 `SFMASK`——它直接从 `R11` 恢复 `RFLAGS`。我们这一章只走 SYSRET 单向进 Ring 3,没有任何 SYSCALL 入口,所以 `SFMASK` 写成什么值,对功能毫无影响。我们照样写了 `0x200`,纯粹是为了将来 023 接 SYSCALL 时不用回来补。调试现场会讲到这个「写了也白写」在 QEMU 上还带来了一个测试上的坑。

## 设计图

先看 STAR MSR 这只 64 位寄存器的位域布局,以及 SYSRET 如何由它推出 Ring 3 的 CS/SS:

```text
        IA32_STAR  (MSR 0xC0000081)   共 64 位
   ┌────────────────────────────────────────────────────┐
   │ 63        48 47        32 31                       0│
   │ ├SYSRET 基址┤ ├SYSCALL 基址┤ ├保留(本 tag 填 0)────┤│
   │   = 0x0008     = 0x0008                              │
   └────────────────────────────────────────────────────┘
        ▲                  ▲
        │                  └─ SYSCALL: CS ← STAR[47:32], SS ← [47:32]+8
        │                     (本 tag 不用, 023 才接)
        │
        └─ SYSRET (REX.W / 64-bit user):
              CS ← STAR[63:48] + 16      = 0x08+0x10 = 0x18, RPL 强制 3 → 0x1B  ← 用户代码段
              SS ← STAR[63:48] + 8       = 0x08+0x08 = 0x10, RPL 强制 3 → 0x13  ← SYSRET 推出的 SS
              RIP ← RCX
              RFLAGS ← R11
              RSP ← (软件自己切, MSR 不管)
```

`STAR[63:48]` 和 `[47:32]` 我们都填了 `0x08`(内核代码段选择子)。SYSRET 方向用高 16 位,推出 `CS=0x1B`、`SS=0x13`——注意这里的 `SS=0x13` 是 SYSRET 硬件按 `STAR[63:48]+8` 再被 CPU 强制 `RPL=3` 算出来的选择子,跟我们 GDT 里那个用户数据段常量 `GDT_USER_DATA=0x23` 不是一回事(0x13 指向 kernel data 段那一项,0x23 才是 user data 段)。long mode 下数据段基址恒为 0,所以这个 `SS=0x13` 在功能上无所谓,host 测试也只断言这一条硬件推导。SYSCALL 方向用 `[47:32]`,这一章用不上,但填上 `0x08` 是给 023 留的口子。

再看 `jump_to_usermode` 在 `sysretq` 之前必须摆好的寄存器契约:

```text
   调用 jump_to_usermode(entry=%rdi, user_stack=%rsi, arg=%rdx)
                          │
   ┌──────────────────────┴───────────────────────────┐
   │ %rcx ← entry      (SYSRET 会把 RCX 装进 RIP)
   │ %rsp ← user_stack (SYSRET 不动 RSP, 必须软件切栈!)
   │ %rdi ← arg        (用户程序入口的第一个参数, ABI 约定 %rdi)
   │ %r11 ← 0x202      (SYSRET 从 R11 恢复 RFLAGS: IF=1 + 保留位1)
   │ %rax,%rbx,%rdx,%rsi,%rbp,%r8..%r15 ← 0   (清干净, 防泄漏内核数据)
   │ sysretq            →  跨进 Ring 3, RIP=entry
   └───────────────────────────────────────────────────┘
```

最后是 Ring 3 触发异常时 CPU 的栈切换路径。这是隔离成立后「墙弹回来」的物理通道:

```text
   Ring 3 执行 cli
        │  CPL=3, 特权指令 → #GP
        ▼
   CPU 查 TSS.RSP0 (本 demo 用当前内核 rsp)
        │
        ├─ 内核栈:  CPU 压入 SS, RSP, RFLAGS, CS, RIP, ERROR_CODE
        │           (此时 SS:RSP 已经是 Ring 3 的, CS 低 2 位 = RPL = 3)
        ▼
   IDT[#GP] → isr_gp_stub → handle_gp(frame)
        │  from_user = (frame->cs & 0x03) != 0  →  true
        ▼
   打印 "#GP ... from user mode (Ring 3)"
   打印 "Privileged instruction executed in Ring 3 -- protection works!"
   fatal_halt()  →  cli; hlt  永久停机

   ── 旁路: 若是 #DF(Double Fault) ──
   CPU 查 TSS.IST1 (tss_.ist[0] → df_stack_[] 独立 4 KB 栈)
   而不是 RSP0 → 烂栈上再炸也不会连环崩
```

本 demo 里 `TSS.RSP0` 直接用了 `launch_first_user` 当时的内核 `rsp`,因为撞墙之后内核就 `halt` 了,不需要操心「从异常返回用户态」那条路——那条路要等 syscall 和真正的进程上下文都齐了才有意义。
