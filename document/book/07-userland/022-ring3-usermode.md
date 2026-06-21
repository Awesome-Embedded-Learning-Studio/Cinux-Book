---
title: 022 · 第一次跳进 Ring 3:用户态与特权隔离
---

# 022 · 第一次跳进 Ring 3:用户态与特权隔离

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

## 代码路线

### usermode_init_asm:三只 MSR 怎么摆

[usermode.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/usermode.S) 的 `usermode_init_asm` 依次写 STAR、SFMASK、EFER。STAR 那段是最容易写错的一句:

```asm
    movq $0x08, %rdx            # 内核 CS 选择子
    shlq $16, %rdx              # 移到 EDX[31:16], 对应 STAR[63:48]
    orq  $0x08, %rdx            # 顺便在 EDX[15:0] 放 0x08, 对应 STAR[47:32]
    xorq %rax, %rax             # 低 32 位 EAX = 0 (STAR[31:0] 不用)
    movq $0xC0000081, %rcx
    wrmsr                       # EDX:EAX → MSR[RCX]
```

为什么是 `shlq $16` 而不是直觉上的 `shlq $32`?因为 `wrmsr` 只认 32 位的 `EDX` 和 `EAX`,把它们的值拼成 `EDX:EAX` 写进 64 位 MSR——它**不读** 64 位 `RDX` 的高 32 位。所以哪怕你 `shlq $32` 把 `0x08` 移到 `RDX` 的高半区,`wrmsr` 也照样只拿 `EDX`(低 32 位)那部分,结果 `STAR[63:48]=0`,`SYSRET` 推出的 CS 就变成 `0x13`(内核数据段 `0x10` | RPL3)——数据段选择子,CPU 在数据段上取指。正确做法是 `shlq $16`,让值落在 `EDX` 的 `[31:16]` 里——这正是 `wrmsr` 会写进 `STAR[63:48]` 的位置。注释里那行 `# %rdx<<16→%rdx: shift to EDX[31:16] for STAR[63:48]` 就是在反复提醒自己这件事。这段逻辑在 host 单元测试里用纯算术镜像过:`(0x08ULL << 16) | 0x08ULL == 0x00080008ULL`。

SFMASK 那段就直白得多,写 `0x200`(屏蔽 IF 位)。但正如上一节说的,SYSRET 不读它,这一章写了也不影响功能;加上 QEMU 的模拟还有坑(调试现场案二),所以测试里只验「写 `0x200` 不 `#GP`」,不断言读回值。

EFER 是读改写:先 `rdmsr` 读出当前值,`orq $1,%rax` 置 `SCE` 位(bit 0),再 `wrmsr` 写回。`SCE` 是 SYSRET/SYSCALL 指令的总开关,不开的话 `sysretq` 直接 `#UD`。

### jump_to_usermode:SYSRET 的寄存器契约

`jump_to_usermode` 按 System V AMD64 约定收参数:`%rdi=entry`、`%rsi=user_stack`、`%rdx=arg`。它把这几个值搬到 SYSRET 指定的位置:

```asm
    movq %rdi, %rcx            # entry → RCX (SYSRET 装进 RIP)
    movq %rsi, %rsp            # user_stack → RSP (SYSRET 不动 RSP, 软件切栈)
    movq %rdx, %rdi            # arg → RDI (用户入口的第一参数)
    pushq $0x202               # RFLAGS: IF(bit9) + 保留位1(bit1)
    popq  %r11                 # → R11 (SYSRET 从 R11 恢复 RFLAGS)
```

这里每一个 `movq` 都对应 SYSRET 硬件契约里的一条,错一个就崩。`movq %rsi,%rsp` 这行尤其要盯住:SDM §5.8.8 明说 SYSRET 不修改 RSP,所以**必须**在 `sysretq` 之前把栈指针切到用户栈,否则用户态会接着用内核栈,隔离和正确性全毁。`pushq $0x202 / popq %r11` 是个把立即数塞进 `R11` 的小技巧——不能直接 `mov` 立即数进 `R11`(虽然能,但用栈更直白),`0x202` 是 `IF` 位(0x200)加上 RFLAGS 里恒为 1 的保留位 1(0x002)。这样用户态一进去中断就是开的(否则第一条指令要是再触发什么异常都收不到)。

紧接着是一串 `xorq %rN,%rN` 把其余通用寄存器清零。这步不是为了 SYSRET,是为了**安全**:内核的寄存器里可能残留着内核地址、内核数据,把这些原样带进 Ring 3 等于把内核信息泄漏给用户态。虽然本 demo 的「用户程序」只是内核里硬编码的四字节、没有恶意,但这个习惯从第一天起就该养成——进 Ring 3 之前,把你不想让用户看到的寄存器全擦干净。

最后 `sysretq` 一跳,CPL 从 0 变 3,RIP=entry,隔离正式生效。

### GDT 用户段 + TSS.RSP0 + IST1

[gdt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/gdt.cpp) 的 `init()` 里,用户代码段、用户数据段这两个描述符其实早就填好了(010 章引入大内核 GDT 时就备好了),`GDT_USER_CODE=0x1B`、`GDT_USER_DATA=0x23` 这两个选择子常量也一直在。这一章新增的是 TSS 这块的实化:

```cpp
// 给 IST1 配一块独立的 4 KB Double Fault 栈
tss_.ist[0] = reinterpret_cast<uint64_t>(&df_stack_[sizeof(df_stack_)]);

const auto tss_addr = reinterpret_cast<uint64_t>(&tss_);
entries_[5] = tss_low_entry(tss_addr, sizeof(TaskStateSegment) - 1);
entries_[6] = tss_high_entry(tss_addr);
```

`df_stack_[]` 是 GDT 类里一块 `alignas(16) uint8_t[4096]` 的静态数组,`tss_.ist[0]` 指向它的**栈顶**(数组末尾,因为栈向下生长)。`TaskStateSegment` 是 104 字节,`static_assert(sizeof(TaskStateSegment)==104)` 把布局锁死;host 测试里镜像了一份 `TestTSS`,断言 `ist` 的偏移是 36、`iomap_base` 是 102、总大小 104——这些数字和 SDM §8.7 的 64 位 TSS 格式(Figure 8-11)对得上。

`tss_set_rsp0` 是个静态方法,写 `g_gdt.tss_.rsp[0]`:

```cpp
void GDT::tss_set_rsp0(uint64_t rsp0) {
    g_gdt.tss_.rsp[0] = rsp0;
}
```

`rsp[0]` 就是 RSP0。为什么这一章非要设它?因为 Ring 3 一旦触发异常(比如我们的 `cli` → `#GP`),CPU 要切回 Ring 0 执行异常处理,而**切到哪个内核栈**就是由 TSS.RSP0 决定的——CPU 从用户栈换到 RSP0 指向的内核栈,在上面压入 `SS/RSP/RFLAGS/CS/RIP/ERROR_CODE`,再跳进 IDT 里的处理程序。RSP0 要是悬空,异常一来栈就乱了。本 demo 用 `launch_first_user` 当时的内核 `rsp` 当 RSP0,够用——撞墙后内核直接 `halt`,不返回用户态,不需要复杂的栈管理。

### launch_first_user:用户地址空间的编排

[usermode.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/usermode.cpp) 的 `launch_first_user()` 是把上面所有零件串起来的编排函数。它分几步走,每一步都回答一个具体问题。

「用户程序」是什么?是硬编码在内核里的四字节字节流:

```cpp
constexpr uint8_t kUserCode[] = {
    0xFA,           // cli   —— 特权指令, Ring 3 执行必 #GP
    0xF4,           // hlt
    0xEB, 0xFC,     // jmp rel8 -4 (跳回 cli, 无限循环)
};
```

如实说:这不是从磁盘加载的 ELF 用户程序,没有 user libc,也没有 `user/linker.ld`——`user/` 目录是 023 才会出现的。这四字节存在的唯一目的,是让 `cli` 在 Ring 3 被执行一次,从而证明特权指令被拦住。

用户地址空间怎么搭?建一个独立的 `AddressSpace`,映一页代码在 `USER_ENTRY_BASE(0x400000)`,映 4 页栈在 `USER_STACK_TOP(0x7FFFFF000)` 下面,全部带 `kUserPageFlags = FLAG_PRESENT|FLAG_WRITABLE|FLAG_USER`:

```cpp
AddressSpace user_space;                              // 独立页表
uint64_t code_phys = g_pmm.alloc_page();
user_space.map(USER_ENTRY_BASE, code_phys, kUserPageFlags);
// ... 把 kUserCode 拷进 code_phys (走内核 higher-half 映射写) ...

uint64_t stack_base = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;
for (i = 0..USER_STACK_PAGES)
    user_space.map(stack_base + i*PAGE_SIZE, g_pmm.alloc_page(), kUserPageFlags);
```

`0x400000` 是 x86-64 上 ELF 默认的加载基址,`0x7FFFFF000` 落在用户半区高位、距离 32 GB 边界(`0x800000000`)只差一页,host 测试把它们都验过页对齐、都在用户半区(`< 0x800000000000`)、彼此不重叠。

接着是这一章最反直觉、也是踩坑最狠的一步——激活用户地址空间前,要把内核 PDPT 里的 **framebuffer identity-mapping** 条目复制进用户 PDPT。`AddressSpace` 构造时只复制了 PML4 的高半区(`[256..511]`,内核共享那部分),低半区全清零。可 framebuffer 用的是 identity mapping(物理地址直接当虚拟地址),落在低半区的 PDPT[3]。一旦 `activate()` 切了 CR3,那条 1 GB 大页就消失了——而 `kprintf` 写 console 正是要访问 framebuffer。后果是 `kprintf` 触发 `#PF`、需求分页又给那地址映了个普通 RAM 页、然后 fault handler 内部又 `kprintf` 又访问 framebuffer……连环 `#PF` 把串口输出搅成一串乱码。所以激活前得手动把缺失的 identity-mapping 条目搬过去:

```cpp
auto* kern_pdpt = ...;  auto* user_pdpt = ...;
for (uint32_t i = 0; i < PT_ENTRIES; i++)
    if ((kern_pdpt[i] & FLAG_PRESENT) && !(user_pdpt[i] & FLAG_PRESENT))
        user_pdpt[i] = kern_pdpt[i];   // 只补用户缺的, 不覆盖
```

之后才是 `user_space.activate()`(切 CR3)、设 `TSS.RSP0=当前 rsp`、调 `jump_to_usermode(USER_ENTRY_BASE, USER_STACK_TOP, 0)`。注意函数最后那句 `kprintf("[USER] ERROR: jump_to_usermode returned!\n")` 在本 demo 里**永远不会执行**——`jump_to_usermode` 一去不回,用户态撞墙后 `fatal_halt()` 永久停机。

### walk_level 的 user_flag:四级页表逐级传 user 位

这是 bug 三的修复,也是最值得讲清的一处。x86-64 的权限检查遍历**全部四级**页表:PML4 → PDPT → PD → PT。一条虚拟地址要让 Ring 3 能访问,不是「最终 PT 项有 user 位」就够——中间任何一级(PML4/PDPT/PD 条目)缺了 user 位,整条路径就判定为「权限不足」,触发 `#PF` 错误码 `0x05`(P=1 页存在、W/R=0 是读、U/S=1 是用户发起)。

原来的 `walk_level` 在分配新的中间页表(PDPT/PD)时只设了 `FLAG_PRESENT|FLAG_WRITABLE`,漏了 `FLAG_USER`。于是哪怕 `VMM::map` 最终给 PT 项带了 `FLAG_USER`,中间几级没有,用户态照样访问不了。修复是给 `walk_level` 加一个 `user_flag` 参数,从 `VMM::map` 提取 `FLAG_USER` 一路传下去:

```cpp
PageEntry* walk_level(PageEntry* table, uint64_t index, bool should_alloc, uint64_t user_flag = 0) {
    // ... 分配新表页时:
    entry.raw = new_page | FLAG_PRESENT | FLAG_WRITABLE | user_flag;
    //                                                ^^^^^^^^^ 逐级传下去
}
```

`VMM::map` 里:

```cpp
uint64_t user_flag = flags & FLAG_USER;     // 从调用者要的 flags 里抠出 user 位
auto* pdpt = walk_level(pml4_table, PML4_INDEX(virt), true, user_flag);
auto* pd   = walk_level(pdpt, PDPT_INDEX(virt), true, user_flag);
auto* pt   = walk_level(pd, PD_INDEX(virt), true, user_flag);
```

这样无论映用户代码页还是用户栈页,从 PML4 到 PT 四级全部带上 user 位,Ring 3 才进得去。这个坑的隐蔽之处在于:它和别的 bug(下面案一里的 framebuffer、STAR 移位)会互相掩盖——中间页表缺 user 位的 `#PF` 先发作,把 STAR 移位错误的症状藏了起来,你以为修好了其实还差一层。

### handle_gp:用 (cs & 0x03) 区分来源

[exception_handlers.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/exception_handlers.cpp) 的 `handle_gp` 这一处改动很小,意义却重:

```cpp
void handle_gp(InterruptFrame* frame) {
    dump_registers(frame, "#GP", 13);
    bool from_user = (frame->cs & 0x03) != 0;     // RPL 低 2 位 != 0 即来自 Ring 3
    if (from_user) {
        kprintf("[EXCEPTION] #GP at RIP=%p from user mode (Ring 3)\n", ...);
        kprintf("[EXCEPTION] Privileged instruction executed in Ring 3 -- protection works!\n");
    } else {
        kprintf("[FATAL] General Protection Fault in kernel mode (error code=%p)\n", ...);
    }
    fatal_halt();
}
```

为什么 `cs & 0x03` 能区分来源?段选择子的低 2 位是 RPL(Requester Privilege Level),异常压栈时 CPU 把当时的 CS(连同 RPL)存进 `InterruptFrame`。Ring 3 里执行指令时 `CS=0x1B`,`0x1B & 0x03 = 3`,非零;内核态 `CS=0x08`,`0x08 & 0x03 = 0`。所以一句位与就能告诉我们「这条 `#GP` 是用户撞墙,还是内核自己出了岔子」。前者正是隔离成立的信号,后者是内核 bug(本 demo 里不该发生)。host 测试把这两个分支都镜像过:`0x1B & 0x03` 判为 user、`0x08 & 0x03` 判为 kernel。

不管哪条分支,最后都 `fatal_halt()`——本 demo 里用户态撞墙就停机,不尝试恢复或杀进程,那是 023 之后的事。

## 调试现场

这一章的两份笔记,一个是「三个 bug 互相掩盖」的连环雷,一个是「模拟器把你的测试骗了」的经典,都值得拆开讲。

### 案例一:进 Ring 3 卡在三连 bug

现象是 `launch_first_user()` 跑完,串口没有预期的 `#GP ... protection works`,反而吐出一串乱码:`[[[[[[[[[[[VMM] Demand-paged 0x... -> phys 0x...`。排查发现三个独立 bug 叠在一起,必须全修才进得了 Ring 3。

根因有仨。第一个,framebuffer 的 identity-mapping 在用户地址空间丢失——`AddressSpace` 只复制高半区,低半区的 1 GB 大页没了,`kprintf` 一写 console 就 `#PF`,需求分页又映个普通 RAM 页,fault handler 内部再 `kprintf` 形成重入,串口被搅烂。第二个,STAR 写入用 `shlq $32`——`wrmsr` 只认 32 位 `EDX`,`RDX` 高半区被丢,`STAR[63:48]=0`,SYSRET 推出 `CS=0x13`(数据段 + RPL3),CPU 在数据段上取指。第三个,`walk_level` 分配中间页表漏了 `FLAG_USER`,四级页表某级缺 user 位,Ring 3 访问被拒 `#PF(0x05)`。

定位过程的关键,是意识到这三个 bug **互相掩盖**。bug 一和 bug 三的 `#PF` 先发作,把 bug 二(STAR 错位)的症状盖住了——你以为修到一半能跑了,其实还差。只有把三个都修:framebuffer identity-mapping 复制进用户 PDPT、`shlq $32` 改成 `shlq $16`、`walk_level` 加 `user_flag` 逐级传,串口才干净地吐出 `Jumping to Ring 3` → `#GP CS=0x001b` → `protection works`。

防复发有三条。一是地址空间切换前,要把所有「内核隐式依赖但不在共享高半区」的映射(像 identity-mapped 的 MMIO)显式继承过去——这是「用物理地址直当虚拟地址」这种 identity mapping 方案在切 CR3 时的固有脆弱点。二是写 64 位 MSR 时心里始终记着 `wrmsr` 只认 `EDX:EAX`,对 `RDX` 做超过 32 位的移位是无效操作。三是 x86-64 权限检查遍历全部四级页表,任何分配新中间页表的代码路径都必须把 user 位传下去——别只在最终叶子节点上带。

### 案例二:QEMU 不持久化 SFMASK 写入

现象是机内测试 `test_sfmask_if_bit` 挂了,断言 `(sfmask & 0x200) == true` 失败:`wrmsr` 写 `0x200` 进 `IA32_FMASK` 后,`rdmsr` 读回是 0。同一函数里 STAR、EFER 的读写都正常。

定位链走得很扎实。先反汇编确认 `usermode_init_asm` 指令序列没错;再把 SFMASK 的写入挪到 EFER 之后,排除「EFER 的 `wrmsr` 覆盖了 SFMASK」;再换 C++ inline asm 直接写,排除汇编链接问题;再在测试函数体内写完立刻读,排除时序。全都失败。最后一步是关键证据:写全 `1`(`0xFFFFFFFF:0xFFFFFFFF`)进 SFMASK,正常触发 **#GP**。这说明 QEMU 认得这只 MSR、也做了合法性检查,只是对**合法值**(如 `0x200`)的写入静默丢弃——`wrmsr` 不报错,值却不落盘。KVM 和 TCG 两种后端一致,确认是 QEMU 本身的模拟行为。

根因是 QEMU 对 `IA32_FMASK` 的模拟不完整。修复不是去改 QEMU,而是改测试的断言口径:从「硬断言读回 `0x200`」改成「写 `0x200` 不触发 `#GP` 就算通过」——能走到 `wrmsr` 之后这一行,就证明指令编码正确。注释里写明:真硬件上 `rdmsr` 应读回 `0x200`。

提炼成教训有两条。一是测试要会区分「模拟器限制」和「真 bug」——当测试结果和代码正确性明显矛盾时,先在模拟器层面排除干扰(这里就是用「写全 1 触发 #GP」反证编码没错)。二是回到设计本身:`SFMASK` 只影响 SYSCALL 方向,而本 milestone 只走 SYSRET、SYSRET 从 `R11` 恢复 `RFLAGS`,所以这只 MSR 写不写得进去,对功能没有任何影响——这个「白写」的事实,恰恰让我们能放心地把测试断言放宽。

## 验证

先在 host 上把纯算术的部分钉死。`ctest -R usermode` 跑 [test_usermode.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_usermode.cpp)(`-DCINUX_HOST_TEST` 门控,不链内核代码),它镜像验证:用户态常量(`USER_ENTRY_BASE=0x400000`、`USER_STACK_TOP=0x7FFFFF000`、`USER_STACK_PAGES=4`)、栈大小与基址(16 KB、`0x7FFFFB000`)、字节码(`cli=0xFA`、`hlt=0xF4`、`jmp -4` 是 `EB FC`)、STAR 计算(`SYSRET CS = 0x08+16+3 = 0x1B`、SS 推出 `0x13`、高 32 位 = `0x00080008`)、RFLAGS `0x202`、用户页标志 `0x7`、镜像 `TestTSS` 的布局(`sizeof=104`、`ist@36`、`iomap_base@102`、`rsp@4`)、以及 `InterruptFrame` 用 `(cs & 0x03)` 判用户态:

```bash
ctest --test-dir build -R usermode --output-on-failure
```

真正的 `SYSRET`/真 GDT/真 IDT/真 PMM/VMM/`AddressSpace`,只能在 QEMU 里验。`run-big-kernel-test` 跑 [test_usermode.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_usermode.cpp),test section 名是 `Usermode Tests (022)`,八组:TSS RSP0 读写、STAR/EFER MSR 读回(`STAR[63:48]=0x08`、`EFER.SCE=1`、SFMASK 仅验「写 `0x200` 不 `#GP`」)、用户 `AddressSpace` 创建/map/translate/隔离、段选择子 inline asm(`mov %cs/%ds/%ss`、`str`)、常量、字节码、IST1 配置、`usermode_init` 已调用:

```bash
cmake --build build --target run-big-kernel-test
```

最后是**生产 demo** 的现象:直接跑大内核,串口应该依次看到 `[USER] Setting up first user-mode program...` → `[USER] User address space activated (PML4 at phys 0x...)` → `[USER] Jumping to Ring 3: entry=0x0000000000400000 stack=0x00000007FFFFF000` → 异常转储里 `CS = 0x001b` → `[EXCEPTION] #GP at RIP=0x... from user mode (Ring 3)` → `Privileged instruction executed in Ring 3 -- protection works!`,然后机器 halt 住。`CS=0x1B`(用户代码段,RPL=3)加上那句 `protection works`,就是本章交付的全部证据。

## 下一站

到这里,我们第一次让 CPU 跑进了 Ring 3,又第一次让一条特权指令撞墙弹回来。隔离的「墙」立起来了——但墙是单面通的:用户态进得去,却没法和内核「说话」。它只能用触发异常这种粗暴的方式引起注意,然后整个机器就停了。

真实的用户态不该这样。用户程序应该能**合法地**请求内核替它做事——往屏幕上写一行字、退出自己、让出 CPU——而不是只能 `cli` 撞墙。这就需要一条从 Ring 3 回到 Ring 0 的「正门」:`SYSCALL` 指令,以及内核里处理它的一套入口。下一站(023)就接这件事:装 syscall 入口、写 `sys_write`/`sys_exit`/`sys_yield` 这几个最基础的系统调用、搭一个最小的 user libc、再让一个真正的 ELF 用户程序(`hello`)跑起来。022 这章立起来的「墙」和那个硬编码的四字节,是那一切的起点——得先证明用户态被关在笼子里,后面给它开一扇合法的门才有意义。

---

### 参考

- **Intel SDM Vol.3A §5.8.8 "Fast System Calls in 64-Bit Mode"**(本地 `document/reference/intel/SDM-Vol3A-System-Programming-Guide-Part1.pdf`,手册 5-22 页 / PDF 第 184 页,本章 `pdf-reader` 实读核实):`SYSRET`(REX.W/64 位用户)的 `CS ← IA32_STAR[63:48]+16`、`SS ← IA32_STAR[63:48]+8`、`RIP ← RCX`、`RFLAGS ← R11`,以及 SYSRET 不修改 RSP、SYSCALL 方向的 `CS ← STAR[47:32]`、`RFLAGS AND NOT IA32_FMASK`。`jump_to_usermode` 的寄存器契约和「软件必须自己切 RSP」的全部依据。
- **Intel SDM Vol.3A §5.8.8 Figure 5-14 "MSRs Used by SYSCALL and SYSRET"**(同 PDF,目录第 36 页列出 Figure 5-14 在 5-23 页):`IA32_STAR` 的 `[63:48]`/`[47:32]` 位域布局、`IA32_FMASK` 仅作用于 SYSCALL 方向——本章 STAR 装配与「SFMASK 写不写无所谓」论断的硬件出处。
- **Intel SDM Vol.3A §5.9 "Privileged Instructions"**(同 PDF,手册 5-23 页起):`HLT`/`WRMSR`/`RDMSR`/`CLTS`/`LTR` 等列名,CPL 非 0 执行即 `#GP`——本章用户字节流 `0xFA(cli)`/`0xF4(hlt)` 在 Ring 3 触发 `#GP`、反向证明隔离的依据。(注:`CLI` 受 `CPL <= IOPL` 约束、用户态 IOPL 通常为 0,故 Ring 3 执行 `cli` 同样 `#GP`;此条 Vol.2A `CLI` 条目,引用时挂 SDM 章节。)
- **Intel SDM Vol.3A §8 Task Management · 64-Bit TSS**(同 PDF,§8.7 Task Management in 64-Bit Mode,Figure 8-11 64-Bit TSS Format):104 字节 TSS 的规范出处,`RSP0`(`rsp[0]`)与 `IST1`(`ist[0]`)字段。本章 `static_assert(sizeof(TaskStateSegment)==104)` 与 host 测试的 `TestTSS` 偏移校验即据此;字段偏移以代码与测试为准,不引死 Figure 编号。
- **Intel SDM Vol.2D `WRMSR` 条目**(本地 `document/reference/intel/SDM-Vol2D-Instruction-Reference-W-Z.pdf`,Chapter 6,手册 6-9 页起):`WRMSR` 把 `EDX:EAX` 写入 `MSR[ECX]`(高 32 位 ← EDX、低 32 位 ← EAX),并对 `RAX`/`RDX` 的高 32 位不予理会——调试现场案一 bug 二(`shlq $32` vs `shlq $16`)的根因依据。
- 018 章 · 地址空间:`AddressSpace` 的「内核半区 `PML4[256..511]` 共享、用户半区私有」设计——`launch_first_user` 为什么要手动把 framebuffer 的 identity-mapping 复制进用户 PDPT,就是对上这个设计。
- 本 tag 源码:[usermode.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/usermode.S) / [usermode.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/usermode.cpp) / [usermode.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/usermode.hpp)、[gdt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/gdt.cpp) / [gdt.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/gdt.hpp)、[idt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/idt.cpp)、[exception_handlers.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/exception_handlers.cpp)(`handle_gp`)、[vmm.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/vmm.cpp)(`walk_level` 的 `user_flag`)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp);测试 [test_usermode.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_usermode.cpp)(host 镜像)、[test_usermode.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_usermode.cpp)(QEMU 机内,section `Usermode Tests (022)`)。
