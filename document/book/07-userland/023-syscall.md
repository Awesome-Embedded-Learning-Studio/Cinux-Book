---
title: 023 · 让用户态会说话:SYSCALL/SYSRET 系统调用
---

# 023 · 让用户态会说话:SYSCALL/SYSRET 系统调用

> 上一章(022)我们终于把脚伸进了 Ring 3——`usermode_init()` 配好 STAR/EFER,`launch_first_user()` 造出一段用户地址空间,`jump_to_usermode` 用 `sysretq` 把 CPU 弹进低特权级。可那条用户程序是**手写的 4 字节机器码**(`cli;hlt;jmp .-2`),除了证明「特权指令在 Ring 3 会触发 #GP」之外,什么都干不了。内核和用户之间没有一条「函数调用」式的受控通道——用户想干点正经事(哪怕只是往屏幕打一行字),都无处下嘴。这一章就把这条路接通:用户程序执行 `syscall` 指令,硬件瞬间把我们送到 Ring 0 的 `syscall_entry`;内核干完活,再用 `sysretq` 把它原样送回 Ring 3。做完,你会看到一行真正由 Ring 3 代码打印的 `[USER] Hello from Ring 3!`——不是内核替它打的,是它自己通过 `sys_write` 请求内核打的。

## 这一章我们要点亮什么

核心是一件:在 Ring 3 和 Ring 0 之间架一条可来回走的、**受控的服务通道**。

具体说,023 交付五块:

- **SYSCALL/SYSRET 机制**:[syscall.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/syscall.S) 里 134 行的 `syscall_entry` 是整章灵魂。它一进来先 `swapgs`、把用户 RSP 藏进 per-CPU 的 `%gs:8`、用 `%gs:0` 载入内核栈,在内核栈上按固定顺序搭一个 12 槽的 trap frame,把第 6 个参数挪到栈上当第 7 个 C 参,按 SysV ABI 重排寄存器后 `call syscall_dispatch`,返回值绕道 `%rbx` 存起来,恢复现场、销毁 frame、切回用户栈、`swapgs` 回去、`sysretq`。配套的 [syscall.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/syscall.cpp) `syscall_init()` 写三只 MSR(STAR/LSTAR/SFMASK),把硬件指向这条入口。
- **dispatch 表**:[syscall_nums.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/syscall_nums.hpp) 定义 `SyscallNr` 枚举(刻意对齐 Linux:`SYS_read=0 / SYS_write=1 / SYS_yield=24 / SYS_exit=60`)、`SYSCALL_TABLE_SIZE=256`、`SyscallFn = int64_t(*)(uint64_t×6)`。`syscall_register` 填表,`syscall_dispatch` 越界或空槽返回 `-1`。
- **三个 handler**:`sys_write`、`sys_exit`、`sys_yield`。其中 `sys_write` 只认 `fd==1`、只做逐字节 `kprintf("%c")`,朴素到近乎寒酸——但够把那句问候打出来。
- **用户态编译基建**:从 022 的「手写 4 字节」升级到「用 C++ 写一个真程序」。[CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/CMakeLists.txt) 把 `hello.cpp` 编成 ELF、`objcopy` 抽成 flat binary、`ld -r -b binary` 包成可链接的 `.o`(`_binary_hello_bin_start/end`),嵌进大内核镜像。
- **顺带打通的 FPU/SSE 与栈对齐**:为了让用户态 C++ 能用 SSE(GCC 把 `const char msg[]` 的初始化优化成 `movaps`),`boot.S` 置 CR0/CR4 开 OSFXSR/OSXMMEXCPT、清 EM/TS;`Task` 加 `alignas(16) uint8_t fpu_state[512]`;调度器三处 `context_switch` 前后配 `fxsave/fxrstor`;用户入口 RSP 用 `USER_ABI_RSP_OFFSET=8` 加一道 `static_assert` 锁死 SysV 对齐。

合起来,这一章给了内核「被用户态请求做事」的能力。但期望要放正:023 是**单任务**——`launch_first_user` 之前没启动调度器,所以 `sys_exit` 实际走的是 `cli;hlt` 死循环分支,不是 yield;没有抢占、没有时钟中断驱动的 syscall 返回。`SyscallNr::SYS_read=0` 这个常量虽然在,但内核侧没人接它——dispatch 到它就返回 `-1`。真正的 read、真正的 shell、真正的常驻进程,是下一站(024)的事。

## 为什么现在需要它

先回答一个一定会冒出来的问题:既然上一章已经能进 Ring 3 了,为什么还要专门搞一套 `syscall`/`sysretq`,而不是直接复用已有的中断机制(`int 0x80` 那种软件中断)?

因为 `syscall` 是为「跨特权级的服务请求」量身定做的,它在三件事上比软件中断干净。其一,它**不查 IDT**——入口地址直接从一个专用 MSR(`LSTAR`)里取,省掉一次查表。其二,它**不压栈**——硬件只把 `RCX ← RIP`、`R11 ← RFLAGS` 两个值塞进寄存器就算完事,连用户 RSP 都不存(`SYSCALL` 对 `RSP` 一字未动,SDM 伪代码白纸黑字)。其三,段选择子也从 MSR(`STAR`)里现算,不需要在中断描述符里编码 DPL。代价是:正因为硬件什么都不替你存,`syscall_entry` 必须自己把「返回地址、返回时的标志、用户栈指针」一个不漏地抢救下来——这就是 trap frame 的由来。

这又引出第二个「为什么」:为什么 `syscall_entry` 第一条指令非得是 `swapgs`,第二条非得是「把用户 RSP 存到 `%gs:8`、用 `%gs:0` 载入内核栈」?

因为 `SYSCALL` 进来时,CPU 还停在用户的 RSP 上、还指着用户的 GS base。我们要立刻切到内核栈去建 trap frame,可内核栈指针存哪儿?不能存进某个固定寄存器——入口这段汇编里每个寄存器都金贵;也不能存进某个内存变量然后直接 `mov`——我们连内核栈都还没换,这时候访问「内核数据结构」语义上就乱了。Intel 给的解法是 `swapgs`:它把 GS.base 和 `KERNEL_GS_BASE` MSR(`0xC0000102`)里的值一交换,GS 立刻指向内核的 per-CPU 区,之后 `%gs:0`、`%gs:8` 这种相对寻址就成了「在内核私有的 scratch 区里读写」,干净利落。所以 per-CPU scratch(那块在 `launch_first_user` 里 `alloc_page` 出来、第一格存 `kernel_rsp0`、整体写进 `KERNEL_GS_BASE` 的页)不是可有可无的装饰,而是 `syscall_entry` 能正常工作的物理前提。

## 设计图

先把三只 MSR 的位布局看清楚——它是 `syscall_init` 的全部输出:

```text
   STAR    (0xC0000081)
   ┌──────────────────────────────────────┬──────────────────────┬──────────────┐
   │ [63:48]  SYSRET CS base  = 0x08      │ [47:32] SYSCALL CS base = 0x08 │ [31:0] 保留 │
   └──────────────────────────────────────┴──────────────────────┴──────────────┘
     SYSRET 时: CS = [63:48] + 16 | RPL3   SS = [63:48] + 8 | RPL3
     SYSCALL 时: CS = [47:32] & FFFC       SS = [47:32] + 8
     → 两槽都填 0x08, SYSCALL 取 0x08
     → SYSRET 算出用户 CS = (0x08+16)|3 = 0x1B (= GDT_USER_CODE)
     → SYSRET 算出用户 SS = (0x08+8)|3  = 0x13 (≠ GDT_USER_DATA 0x23)

   LSTAR   (0xC0000082)   RIP ← LSTAR        即 syscall_entry 的地址
   SFMASK  (0xC0000084)   RFLAGS ← RFLAGS AND NOT SFMASK    =0x200 → 入口清 IF
```

再看 `syscall_entry` 一进一出到底干了什么:

```text
   Ring 3: syscall                        Ring 0: syscall_entry
   ──────────────                         ──────────────────────────────────────────
   硬件替你做的 (SYSCALL 伪代码):           ① swapgs                         (GS↔KERNEL_GS)
     RCX ← RIP                            ② mov %rsp,%gs:8 ; mov %gs:0,%rsp (存用户RSP,换内核栈)
     R11 ← RFLAGS                         ③ push 12 槽 trap frame (见下)
     RIP ← LSTAR                          ④ push arg6 (第7个 C 参上栈)
     CS ← STAR[47:32], SS ← STAR[47:32]+8 ⑤ 重排 6 参到 rdi/rsi/rdx/rcx/r8/r9 (偏移 +8)
     RFLAGS ← RFLAGS AND NOT SFMASK       ⑥ call syscall_dispatch  →  返回值在 %rax
     RSP: 硬件一概不动!                  ⑦ add $8,%rsp  (丢掉第7参)
                                         ⑧ mov %rax,%rbx  (返回值绕道 rbx 存)
                                         ⑨ 从 frame 恢复 %rcx(user RIP)、%r11(user RFLAGS)
                                         ⑩ 记回用户 RSP 到 %gs:8
                                         ⑪ add $96,%rsp  (销毁 12 槽 frame)
                                         ⑫ mov %gs:8,%rsp  (切回用户栈)
                                         ⑬ mov %rbx,%rax  (还返回值)
                                         ⑭ swapgs  (GS 换回用户侧)
                                         ⑮ sysretq  →  RCX→RIP, R11→RFLAGS, 回 Ring 3
```

内核栈上那个 12 槽 trap frame 的精确布局,是汇编和后续取参之间的契约——偏移错一格,整条链就乱:

```text
   内核栈 (push 顺序: rbp 在最底, user_rsp 在最顶)
   ┌─────────────────────────────────────────┐
   │ rsp+ 0:  user RSP   (最后 push, 在顶)   │
   │ rsp+ 8:  user RIP   (RCX, syscall 存的) │
   │ rsp+16:  user RFLAGS(R11, syscall 存的) │
   │ rsp+24:  syscall 号 (RAX, 亦是返回值位) │
   │ rsp+32:  arg1 (RDI)                      │
   │ rsp+40:  arg2 (RSI)                      │
   │ rsp+48:  arg3 (RDX)                      │
   │ rsp+56:  arg4 (R10)  ← 注意是 R10 不是 RCX│
   │ rsp+64:  arg5 (R8)                       │
   │ rsp+72:  arg6 (R9)                       │
   │ rsp+80:  callee-saved RBX                │
   │ rsp+88:  callee-saved RBP (最先 push, 底)│
   └─────────────────────────────────────────┘
        push 顺序倒过来看: 先 push %rbp → 落在最底(+88),
        依次往上, 最后 push user_rsp → 落在最顶(+0)
```

最后是从 `hello.cpp` 的 `sys_write(1, msg, 26)` 到串口冒出字符的完整一跳:

```text
   hello.cpp (_start)                    用户态
     │ sys_write(1, msg, 26)
     │   └─ _syscall3: rax=1 rdi=1 rsi=&msg rdx=26 ; syscall
     ▼
   [SYSCALL 指令]                        硬件: RCX←RIP R11←RFLAGS RIP←LSTAR CS/SS←STAR
     ▼
   syscall_entry                         Ring 0
     │ swapgs ; 换栈 ; 建 frame ; 重排参 ; call syscall_dispatch
     ▼
   syscall_dispatch(nr=1, ...)           命中 syscall_table[1] = sys_write
     ▼
   sys_write(fd=1, buf_virt=&msg, 26)    校验 buf_virt < 0x800000000000 且 fd==1
     │ for each byte: kprintf("%c", buf[i])
     ▼
   串口 + Console                         [USER] Hello from Ring 3!
     │ return 26
     ▼  (原路返回: rbx→rax ; 恢复 rcx/r11 ; 切栈 ; swapgs)
   [SYSRETQ]                             硬件: RIP←RCX RFLAGS←R11 回 Ring 3
     ▼
   hello.cpp 继续 → sys_exit(0) → ... → [SYSCALL] sys_exit: no scheduler, halting.
```

## 代码路线

### 三只 MSR:SYSCALL 怎么知道往哪跳、用什么段、清哪些 flag

[syscall.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/syscall.cpp) 的 `syscall_init` 干的事,翻译成人话就是「给硬件留三个地址」。看核心几行:

```cpp
constexpr uint32_t MSR_STAR   = 0xC0000081;
constexpr uint32_t MSR_LSTAR  = 0xC0000082;
constexpr uint32_t MSR_SFMASK = 0xC0000084;

uint64_t star_val = (static_cast<uint64_t>(GDT_KERNEL_CODE) << 32)
                  | (static_cast<uint64_t>(GDT_KERNEL_CODE) << 48);
write_msr(MSR_STAR, star_val);                       // 两槽都填 0x08
write_msr(MSR_LSTAR, reinterpret_cast<uint64_t>(syscall_entry));
write_msr(MSR_SFMASK, 0x200);                        // bit9 = IF
```

`STAR` 把同一个值 `0x08` 同时塞进 `[47:32]`(SYSCALL 取)和 `[63:48]`(SYSRET 取)两个槽。为什么都填 0x08?因为 SYSCALL 进来要用它当**内核**代码段(`CS = STAR[47:32] & FFFC = 0x08`),而 SYSRET 出去时,硬件拿 `[63:48]` 算**用户**段:`CS = (STAR[63:48] + 16) | RPL = (0x08 + 16) | 3 = 0x1B`、`SS = (STAR[63:48] + 8) | RPL = (0x08 + 8) | 3 = 0x13`。用户 CS 这半句算下来正好等于 `GDT_USER_CODE`(0x1B),没问题;可用户 SS 算出来是 0x13,并不等于 `GDT_USER_DATA`(0x23)。这是 023 这套 STAR 取值下没对齐的一处——单任务跑 `SYSCALL→sys_write→SYSRETQ` 往返时,因为同一段寄存器一直是这个值、没人另设 SS,它能蒙混过去;可一旦多任务或中断往返把 SS 换成别的值,这 0x13 就会咬人。这个坑怎么定位、怎么修,是下一站(024)的调试现场,这里只点破它没对齐,不展开。

`LSTAR` 直接指向 `syscall_entry` 的地址,这就是 SYSCALL 的落点。`SFMASK=0x200` 让硬件在入口执行 `RFLAGS ← RFLAGS AND NOT 0x200`,即把 IF 清掉——syscall 进来那一刻中断是关的,免得 trap frame 还没建好就被时钟中断打断。

这里有个容易踩混的点,得专门说清:**STAR 在 023 被写了两次**。`usermode_init()`(在汇编 `usermode_init_asm` 里,先于 `syscall_init` 调用)也写了一遍 STAR——它用 `movq $0x08,%rdx; shlq $16,%rdx; orq $0x08,%rdx` 把同样的 `0x08/0x08` 拼进去。两边写法不同(汇编靠移位、C++ 靠字面量),但**值完全一致**。`main.cpp` 里的调用序是先 `usermode_init()` 再 `syscall_init()`,所以最终生效的是后者那一次——但因为两者目标一致,谁最后写都一样。把这件事想明白,就不会在调试时困惑「我明明在 syscall.cpp 里改了 STAR,为什么读回来是另一个值」。

### syscall_entry:swapgs、换栈、建 trap frame

[syscall.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/syscall.S) 的开头,是整段最容易写错的部分:

```asm
syscall_entry:
    swapgs                              # GS.base ↔ KERNEL_GS_BASE
    movq %rsp, %gs:8                    # 把用户 RSP 藏进 per-CPU scratch
    movq %gs:0, %rsp                    # 载入内核栈指针
```

为什么是 `swapgs` 而不是先碰栈?因为这一刻 CPU 还在用户的 `RSP` 上、`GS` 还指着用户侧。要是先 `mov %rsp,...` 把用户 RSP 存到某个「内核变量」里,语义上就是「在用户上下文里访问内核数据」,乱套。`swapgs` 先把 GS 换成内核的 per-CPU 基址,之后的 `%gs:0`、`%gs:8` 就稳稳落在内核私有的 scratch 页上。那个 scratch 页是在 `launch_first_user` 里现造的:分配一页物理页,第一格 `gs_virt[0] = kernel_rsp0` 存当前内核栈顶,第二格清 0,然后 `wrmsr(MSR_KERNEL_GS_BASE 0xC0000102, gs_virt)` 把它登记成 KERNEL_GS_BASE。没有这一步,`swapgs` 之后 `%gs:0` 读出来的就是垃圾,栈一换就炸。

换好栈,开始建 frame。注意 push 顺序和最终偏移是**反过来**的——先 push 的落到栈底(高地址):

```asm
    pushq %rbp        # 落到 +88 (callee-saved, 先存)
    pushq %rbx        # +80
    pushq %r9         # +72  arg6
    pushq %r8         # +64  arg5
    pushq %r10        # +56  arg4  ← 注意是 R10
    pushq %rdx        # +48  arg3
    pushq %rsi        # +40  arg2
    pushq %rdi        # +32  arg1
    pushq %rax        # +24  syscall 号
    pushq %r11        # +16  user RFLAGS
    pushq %rcx        # +8   user RIP
    movq %gs:8, %rax  # 取回刚才存的用户 RSP
    pushq %rax        # +0   user RSP (最后 push, 在顶)
```

第 4 个参数存的是 `%r10`,不是 `%rcx`——这是 SYSCALL 调用约定和普通 SysV C ABI 的关键分歧。SysV 说 C 函数第 4 个参数走 `%rcx`;可 SYSCALL 硬件把 `RCX` 抢去存返回地址了,于是用户态约定把第 4 参挪到 `R10`。frame 里如实记成 R10,等会儿 dispatch 时再挪回 rcx。

### 从 trap frame 到 C 函数:第 7 个参数的栈上挪位

`syscall_dispatch` 是个普通的 SysV C 函数,签名是 7 个参数:`(nr, a1..a6)`。前 6 个走寄存器(`rdi/rsi/rdx/rcx/r8/r9`),第 7 个得上栈。可我们的 6 个参数在 frame 里的位置,和 SysV 要求的寄存器对不上——frame 里 arg4 在 R10、arg5 在 R8、arg6 在 R9,而 SysV 要 arg4 走 rcx、arg5 走 r8、arg6 走 r9。所以得重排,这一段是整条链里最绕的:

```asm
    movq 72(%rsp), %rax      # 取 frame+72 = arg6 (原 R9)
    pushq %rax               # 先把它压栈, 当第 7 个 C 参 → 此后所有 frame 偏移 +8!

    movq 32(%rsp), %rdi      # +32: 注意! 原来 nr 在 +24, push 了一格后变 +32
    movq 40(%rsp), %rsi      # arg1 (原 RDI)
    movq 48(%rsp), %rdx      # arg2 (原 RSI)
    movq 56(%rsp), %rcx      # arg3 ← frame 里的 RDX (原 arg3 原样)
    movq 64(%rsp), %r8       # arg4 ← frame 里的 R10 (原 arg4)
    movq 72(%rsp), %r9       # arg5 ← frame 里的 R8  (原 arg5)

    call syscall_dispatch
    addq $8, %rsp            # call 返回, 把第 7 参从栈上抹掉
```

为什么 `push %rax` 之后,后面取参的偏移全都 +8?因为 push 让 RSP 减了 8,frame 整体相对 RSP 往高挪了一格。原本 nr 在 `+24`,push 后就跑到 `+32` 了——这不是笔误,是栈生长方向的必然。把这点想通,就不会写出「nr 取错位、dispatch 到了乱七八槽的号」的 bug。

还有一处容易读漏:上面那 6 条 `mov` 只把 nr、arg1..arg5 喂进了寄存器(`rdi/rsi/rdx/rcx/r8/r9`),第 6 个 C 参(r9)拿的其实是 frame 里的 R8(原 arg5),而不是 arg6。真正的 arg6(frame 里的 R9)是靠开头那条 `mov 72(%rsp),%rax; push %rax` 单独上栈当第 7 个 C 参的——它在 push 之前、frame 还没 +8 时就读走了 `+72` 的旧值(=arg6 R9),所以不走这 6 条 mov。

还有一处要盯死:`nr` 取的是 `frame+32`(即 `+24` 的 syscall 号),但 SysV 要求 C 的**第 1 个参数**走 `rdi`。所以这里 `mov 32(%rsp),%rdi` 是把「syscall 号」当第 1 参传进 `syscall_dispatch(nr, a1..a6)`。frame 里的 arg1(RDI)反而成了 C 的第 2 参(走 `rsi`)。编号错位一位,是「syscall 号要占住第 1 参位」的代价。

### 返回路径:为什么返回值要绕道 rbx

`syscall_dispatch` 返回后,返回值在 `%rax`。按理直接 `sysretq` 就完事——SYSRETQ 本来就从 `rax` 取返回值。可问题是:在「拿到返回值」和「执行 sysretq」之间,还有一连串指令要动寄存器——要恢复 user RIP(进 rcx)、user RFLAGS(进 r11)、要销毁 frame(`add $96`)、要切回用户栈(`mov %gs:8,%rsp`)。这些指令但凡有一条顺手用了 rax,返回值就没了。

解法是把返回值先寄存到一个 SYSRETQ 绝不会碰的寄存器里。callee-saved 的 `%rbx` 是天然人选——SYSRETQ 的语义(`RIP←RCX`、`RFLAGS←R11`、CS/SS 从 STAR 算)压根不提 rbx:

```asm
    movq %rax, %rbx          # 返回值先寄存到 rbx (SYSRETQ 不碰它)

    movq 0(%rsp), %rax       # 取 user RSP
    movq %rax, %gs:8         # 记回 scratch (待会儿切栈要用)
    movq 8(%rsp), %rcx       # 恢复 user RIP (SYSRETQ 从 rcx 取)
    movq 16(%rsp), %r11      # 恢复 user RFLAGS (SYSRETQ 从 r11 取)

    addq $96, %rsp           # 销毁整个 12 槽 frame
    movq %gs:8, %rsp         # 切回用户栈
    movq %rbx, %rax          # 把返回值从 rbx 还回 rax

    swapgs                   # GS 换回用户侧 (入口换过一次, 出口必须换回来)
    sysretq
```

`swapgs` 入口一次、出口一次,必须配对——入口把 GS 从用户侧换到内核侧,出口得再换回去,否则下次进用户态 GS 就指错地方了。「切回用户栈」(`mov %gs:8,%rsp`)必须在 `sysretq` **之前**:因为 SYSRETQ 不改 RFLAGS 里的 TF/IF 之外的栈语义、更不动 RSP,你给它什么 RSP,它就在什么 RSP 上回用户态。要是在切栈之前就 `sysretq`,用户态一返回就踩在自己的栈帧之外,立刻炸。

这一版「返回值绕道 rbx」是 023 的实际写法,它在当前这套 GDT 布局下能跑通。它是不是「最终最优」,这一章不下结论——那是后续要打磨的地方。

### dispatch 表 + 三个 handler

[syscall_nums.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/syscall_nums.hpp) 的枚举刻意对齐 Linux x86-64 的号(read=0、write=1、yield=24、exit=60)。这么做的好处不是「能跑 Linux 程序」(差得远),而是让以后真要移植用户程序时,号能对上、少改一处。`SYSCALL_TABLE_SIZE=256` 给了足够的槽,`SyscallFn` 统一成「6 个 uint64 进、一个 int64 出」的函数指针——所有 handler 签名一致,dispatch 才能用一张表统一管。

`syscall_dispatch` 本身极简:越界(`nr >= 256`)返回 `-1`,空槽(`table[nr]==nullptr`)打一行 `[SYSCALL] unhandled` 也返回 `-1`,否则 `table[nr](a1..a6)`。两个 handler 值得单独看。

[sys_write.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/sys_write.cpp) 是「朴素到寒酸」的典范:

```cpp
constexpr uint64_t USER_ADDR_MAX = 0x800000000000ULL;

int64_t sys_write(uint64_t fd, uint64_t buf_virt, uint64_t count,
                  uint64_t, uint64_t, uint64_t) {
    if (buf_virt >= USER_ADDR_MAX) return -1;   // 拒绝内核地址
    if (fd != 1) return -1;                      // 只认 stdout
    const auto* buf = reinterpret_cast<const char*>(buf_virt);
    for (uint64_t i = 0; i < count; i++) kprintf("%c", buf[i]);
    return static_cast<int64_t>(count);
}
```

两道校验:地址上界 `0x800000000000`(canonical address 的分水岭,高于它的就是内核半区,用户不该传)、`fd==1`。它没有 VFS、没有 fd 表、没有缓冲区、没有真正的「写文件」——就是逐字节 `kprintf("%c")` 把字符往串口和 Console 送。这距离 Linux 的 `write(2)` 差着十万八千里,但对 023 的目标(证明通道通)够用了。注意那道地址校验只是「上界」,不是真正的 `copy_from_user`:它不检查页是否映射、不处理缺页。用户传个没映射的地址进来,`kprintf` 读到那字节时会缺页——那是 023 留着的口子。

[sys_exit.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/sys_exit.cpp) 有一处必须说清的「设计性分歧」:

```cpp
auto* task = Scheduler::current();
if (task != nullptr) task->state = TaskState::Dead;

if (Scheduler::is_initialized()) {
    Scheduler::yield();                 // 调度器在: 让出 CPU
} else {
    kprintf("[SYSCALL] sys_exit: no scheduler, halting.\n");
    while (1) asm volatile("cli; hlt"); // 调度器没启: 死循环停机
}
```

`launch_first_user` 之前,`main.cpp` **没有**调 `Scheduler::init()`——020 写好的调度器在这条路径上压根没启动。所以 023 跑生产 demo 时,`sys_exit` 走的是 `else` 分支:`cli;hlt` 死循环,串口收尾是那句 `[SYSCALL] sys_exit: no scheduler, halting.`。这不是 bug,是刻意的解耦:`yield` 那条分支是「为 024 留的、本 tag 跑不到」的代码。这样写的好处是 syscall 模块在「有调度器」「无调度器」两种环境都能干净收场,不把里程碑之间的耦合硬拧在一起。

### 用户态编译基建:从 hello.cpp 到嵌入内核的镜像

022 的用户程序是 4 字节机器码,023 把它换成了一个真 C++ 程序。这套基建是 [CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/CMakeLists.txt) 三步搭出来的:

第一步,把 [hello.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/programs/hello.cpp) 编成 ELF——用 `-mcmodel=small`(用户态在低 2GB)、`-ffreestanding -nostdlib -static -fno-pie`,链接脚本 [linker.ld](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/linker.ld) 把 `USER_VMA` 定在 `0x400000`、`.text.start` 段放最前:

```cpp
extern "C" void _start() {
    const char msg[] = "[USER] Hello from Ring 3!\n";
    sys_write(1, msg, 26);
    sys_exit(0);
}
```

入口为什么是 `_start` 而不是 `main`?因为我们 `-nostdlib`,没有 libc 的 crt 帮你调 `main`——`_start` 是 ELF 的 `ENTRY`,内核跳进来就直接落在这儿。`sys_exit(0)` 之后那行 `__builtin_unreachable()`(在 [syscall.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/libc/syscall.cpp) 的封装里)是给编译器的承诺:`sys_exit` 不会返回,别在后面排什么清栈指令。

第二步,`objcopy -O binary` 把 ELF 抽成 flat binary(剥掉 ELF 头,只留可执行字节,加载到 `0x400000` 就能跑)。第三步最巧妙:用 `ld -r -b binary hello.bin` 把这个 flat binary 包成一个可链接的 `.o`,它会自动生成 `_binary_hello_bin_start` / `_binary_hello_bin_end` 两个符号。[usermode.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/usermode.cpp) 就靠这两个符号把用户程序字节逐个拷进分配好的用户代码页:

```cpp
extern const uint8_t _binary_hello_bin_start[];
extern const uint8_t _binary_hello_bin_end[];
// ...
size_t user_size = _binary_hello_bin_end - _binary_hello_bin_start;
auto* code_virt  = reinterpret_cast<uint8_t*>(code_phys + KERNEL_VMA);
for (size_t i = 0; i < user_size; i++) code_virt[i] = _binary_hello_bin_start[i];
```

用户态那侧的 `syscall` 封装也是手写的:[syscall.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/libc/syscall.cpp) 的 `_syscall3` 是一段内联汇编,把号塞 `rax`、参数塞 `rdi/rsi/rdx`,执行 `syscall`,clobber 列里老老实实写上 `rcx`、`r11`、`memory`——因为 SYSCALL 会破坏 rcx(存了 RIP)和 r11(存了 RFLAGS),不声明 clobber,编译器会以为这俩寄存器跨调用不变,优化出灾难。

### 顺带打通的 FPU/SSE 与栈对齐

这一块不是 syscall 的本职,但没有它,`hello.cpp` 根本跑不到 `sys_write` 那一行。根因是 GCC 会把 `const char msg[] = "..."` 的初始化优化成 SSE 的 `movaps`——而 `movaps` 要求目标 16 字节对齐,不对齐就 `#GP`。

打开 FPU 是 [boot.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/boot.S) 的事:置 CR4 的 OSFXSR(bit9)+ OSXMMEXCPT(bit10)、清 CR0 的 EM(bit2)、置 MP(bit1)、`clts` 清 TS。这告诉 CPU「操作系统支持 SSE、会用 FXSAVE/FXRSTOR 保存 SSE 状态、别替我仿真 x87」。配套地,`Task` 结构体加了 `alignas(16) uint8_t fpu_state[512]`——FXSAVE 恰好写 512 字节、且目标必须 16 字节对齐否则自身就 #GP;`TaskBuilder::build()` 里 `fninit + fxsave` 给每个任务初始化一份干净状态;[scheduler.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/scheduler.cpp) 的 `schedule`/`exit_current`/`run_first` 三处都在 `context_switch` 前后配 `fxsave`(存当前)/`fxrstor`(恢复下一个),保证任务切换时 SSE 状态跟着走。

光开 FPU 还不够——`movaps` 仍然 #GP,真因是栈不满足 SysV 对齐。ABI 要求函数入口 `RSP ≡ 8 mod 16`(模拟 `call` 压入 8 字节返回地址后栈 16 对齐)。可 Cinux 的 `USER_STACK_TOP = 0x7FFFFF000` 本身是 `0 mod 16`,`sub rsp,0x28` 之后变成 `8 mod 16`,`movaps` 当场炸。修复是在跳转前把 RSP 减 8,并锁一道编译期断言:

```cpp
constexpr uint64_t USER_ABI_RSP_OFFSET = 8;
static_assert((USER_STACK_TOP - USER_ABI_RSP_OFFSET) % 16 == 8,
              "User entry RSP must satisfy x86_64 ABI alignment");
// launch_first_user 里:
jump_to_usermode(USER_ENTRY_BASE, USER_STACK_TOP - USER_ABI_RSP_OFFSET, 0);
```

ABI 对齐是编译期就该锁死的契约,不该等运行时 `#GP` 了才发现。这道 `static_assert` 把它焊死。

## 调试现场

这一章的调试笔记里躺着三个坑,前两个是「症状误导」的典型,第三个是「设计性而非 bug」的代表。

### 案例一:用户态 movaps #GP——病因叠了两层

症状:用户态 C++ 一跑到 `movaps XMMWORD PTR [rsp], xmm0` 就 `#GP`,`RIP=0x400019`、`RSP=0x7FFFFEFD8`。第一反应是「FPU/SSE 没开」——于是去 `boot.S` 加 CR0/CR4 初始化。开了再跑,还是 `#GP`。

这里就卡住了。根因其实叠了两层:第一层确实是 FPU/SSE 没启(已修),但修完仍炸,说明还有第二层——栈不满足 SysV 对齐。`0x7FFFFEFD8` 是 `8 mod 16`,而 `movaps` 要 `0 mod 16`。修复是 `USER_ABI_RSP_OFFSET=8` + `static_assert`。教训是:**一个 `#GP` 可能同时叠了两层病因**,定位时必须分开验证——别因为「开了 FPU 还炸」就否定 FPU 那层,也别因为「FPU 是病因之一」就以为修完它就万事大吉。把对齐单独拎出来、用反汇编里 `sub rsp,0x28` 后的实际 RSP 值去对 ABI 条款,才看得清第二层。

### 案例二:加 FPU init 后,169 个大内核测试全跳过

症状:在 `boot.S` 加完 FPU 初始化,跑 `make run-kernel-test` 直接报 `Loaded ELF is not a real kernel, exiting`,169 个机内测试一个都没跑。

根因不在 FPU 逻辑本身,而在「启动指令的字节序列」。[main_test.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/test/main_test.cpp) 用大内核入口的**前 3 个字节**验真:它要求是 `FA 48 BC`(或 C7),即 `cli` + `mov rsp, imm`。原始 `boot.S` 头两条正是这个序列。可 FPU 初始化一插,变成了 `cli` + `mov %cr4,%rax`(字节 `FA 0F 20`),验真立刻判否。修复是把 FPU init 挪到「栈设置之后」,保住前两条指令的字节模式。教训是:改启动汇编要盯死那些「被外部工具当签名校验」的字节序列——你以为只是调换了下指令顺序,对校验方来说就是「整个内核不像真的了」。

### 案例三:sys_exit 走 halt 而非 yield

症状:生产 demo 跑完 `Hello from Ring 3!`,串口最后打的是 `[SYSCALL] sys_exit: no scheduler, halting.`,机器就此停住,而不是切到别的进程。

这不是 bug,是设计。本 tag 在 `launch_first_user` 之前没启动调度器,`Scheduler::is_initialized()` 返回 false,`sys_exit` 走 `cli;hlt`。那条 `yield` 分支是「为 024 留的、本 tag 跑不到」的代码。这种「跨里程碑解耦」的代价就是:syscall 模块要在「无调度器」下也能干净收场。要是图省事直接 `yield()`,在调度器没启时会崩得更难看。把双分支写明白、配上那行提示日志,是让「这是预期行为」变得可读可查。

## 验证

syscall 的纯逻辑(号常量、dispatch 表的 register/覆写/越界/空槽、`sys_write` 的 fd 与地址校验、`sys_exit` 的 state→Dead、STAR 值计算、SyscallFn 签名一致性)在 host 上镜像着测。[test_syscall.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_syscall.cpp) 把 `SyscallNr`、dispatch 表、`sys_write`/`sys_exit`/`sys_yield` 的逻辑在 host 侧重写了一份(不链内核代码,`CINUX_HOST_TEST` 门控):

```bash
ctest --test-dir build -R syscall --output-on-failure
```

真正的 MSR 写入和真 dispatch(真 `wrmsr`、真 `syscall_register`、越界 256/1024 返回 -1、slot 255 最大合法、`sys_write` 直调 fd≠1 与 `buf_virt≥0x800000000000` 返回 -1)只能在 QEMU 里验。[test_syscall.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_syscall.cpp) 在机内跑,节名 `Syscall Tests (023)`:

```bash
cmake --build build --target run-big-kernel-test
```

机内会用 `rdmsr` 读回 LSTAR≠0、STAR 的 `[47:32]` 和 `[63:48]` 都是 `0x08`、`SFMASK` 写 `0x200` 不 #GP,并验证 `syscall_get_kernel_rsp()` 非零——这些是「硬件真把 MSR 接上了」的直接证据。

最后是**生产 demo**:直接跑大内核(`cmake --build build --target run`,或对应 QEMU 目标),串口应该依次出现:

```text
[USER] Jumping to Ring 3: entry=0x0000000000400000 stack=0x00000007FFFFF000
[USER] Hello from Ring 3!
[SYSCALL] sys_exit: no scheduler, halting.
```

第一行是 `launch_first_user` 报告的跳转参数,第二行就是 `hello.cpp` 通过 `sys_write` 真正打出来的(逐字节经 `kprintf` 落到串口),第三行是 `sys_exit` 在无调度器下的收尾。这三行齐了,说明「用户 `syscall` → 内核 dispatch → `sysretq` 回用户」的整条往返跑通了。

## 下一站

到这里,用户态第一次有了「跟内核说话」的嘴——`sys_write` 能把字打到屏幕上。可它的嘴只张了一下就 `exit` 了。没有 `sys_read`,用户程序听不见键盘;没有 shell,它不能常驻、不能交互地等你输命令;`sys_exit` 走的是 halt,一退整个机器就停了。

下一站(024)就补这三件事:接上 `sys_read`(真从键盘读输入)、写一个常驻的 shell(echo/help/clear 那一套)、并在 `launch_first_user` 之前启动调度器,让 `sys_exit` 走 yield、shell 能作为常驻进程一直在那儿。顺带——SYSCALL/SYSRET 这套机制一旦真用起来,会暴露两个 023 单任务时碰不到的真坑:SYSRET 的 SS RPL 问题、`syscall_entry` 里 rbx 的 clobber。那两个坑怎么定位、怎么修,是下一章的调试现场。023 把「通道」打通了,024 才有底气往这条通道上塞真东西。

---

### 参考

- **Intel SDM Vol.3A §5.8.8 "Fast System Calls in 64-Bit Mode"**(本地 `document/reference/intel/SDM-Vol3A-System-Programming-Guide-Part1.pdf`,PDF 第 184 页前后,本章 `pdf-reader` 实读核实 §5.8.8 语境与 Figure 5-14):SYSCALL 把返回地址存 `RCX`、`RFLAGS` 存 `R11`、`RIP ← IA32_LSTAR`、目标 `CS ← STAR[47:32]`、`SS ← STAR[47:32]+8`、`RFLAGS AND NOT IA32_FMASK`(故 `SFMASK=0x200` 清 IF);SYSRET 取 `STAR[63:48]+16/+8`、`RIP ← RCX`、`RFLAGS ← R11`;**SYSCALL/SYSRET 都不动 RSP**——`syscall_entry` 的换栈、恢复 `rcx/r11`、入口清 IF、`sysretq` 返回的全部依据。指令级伪码另见 Vol.2B `SYSCALL`/`SYSRET` 条目(本地 `SDM-Vol2B-Instruction-Reference-M-U.pdf`),可交叉对照。
- **Intel SDM Vol.3A · SWAPGS / IA32_KERNEL_GS_BASE**(本地同 PDF,PDF 第 99 页,本章 `pdf-reader` 实读核实):SWAPGS 交换 GS.base 与 `IA32_KERNEL_GS_BASE`(MSR `0xC0000102`),无寄存器/内存操作数——「SYSCALL 入口第一条必须 swapgs、用 `%gs:0/%gs:8` 拿 per-CPU 内核栈」的全部理由;对应 `launch_first_user` 里分配 GS 页 + `wrmsr(KERNEL_GS_BASE, ...)`。
- **Intel SDM Vol.3A · CR4.OSFXSR / FXSAVE**(本地同 PDF,本章 `pdf-reader` 实读核实):「启用 x87/SSE」步骤 1「置 `CR4.OSFXSR[bit 9]=1`」见 PDF 第 486 页、CR4 位说明见第 80 页、`OSFXSR`/`OSXMMEXCPT`/`EM`/`TS` 组合表 Table 14-1/14-2 见第 487/488 页。这里要把两类异常分开记清楚:在 `OSFXSR=0`(或 `CR0.EM=1`)下执行 SSE 指令,触发的是 invalid-opcode `#UD`(白纸黑字见 PDF 第 78 页「SSE/.../SSE4 instructions causes an invalid opcode exception (#UD)」,Table 14-1 的 `OSFXSR=0` 行也是 `#UD`);而本章「案例一」里那个 `movaps #GP` 是另一回事——SSE 的内存访问指令要求目标地址 16 字节对齐,对齐失败才 `#GP`(指令级条款见 Vol.2A `movaps` 条目)。也就是说 `OSFXSR=0` 焊的是 `#UD`,`movaps` 栈未对齐焊的是 `#GP`,两者别混。`boot.S` 置 OSFXSR/OSXMMEXCPT、清 EM/TS、`Task.fpu_state[512]` + `alignas(16)` + 调度器 `fxsave/fxrstor` 的依据也都在这一节;`FXSAVE` 保存区 512 字节、目标须 16 字节对齐的条款,另见 Vol.2A `FXSAVE` 条目(本地 `SDM-Vol2A-Instruction-Reference-A-L.pdf`)。
- **Linux man-pages · `syscall(2)`**([man7.org](https://man7.org/linux/man-pages/man2/syscall.2.html),本章 `fetchWebContent` 实读核实):架构表 x86-64 行用 `syscall` 指令、syscall 号在 `rax`、返回值在 `rax`,第二张表 arg1..arg6 = `rdi/rsi/rdx/r10/r8/r9`——**arg4 用 `r10` 而非 `rcx`(因 `rcx` 被 SYSCALL 抢去存返回地址)**的权威出处,Cinux `SyscallNr` 对齐 Linux、`syscall_entry` 把 arg4 从 `R10` 挪进 `rcx` 的依据。
- **System V AMD64 ABI**([x86-psABIs/x86-64-ABI](https://gitlab.com/x86-psABIs/x86-64-ABI),019 章已 live 核,沿用):参数寄存器顺序 `rdi/rsi/rdx/rcx/r8/r9`、第 7 参进栈、callee-saved(`rbx/rbp/r12-r15`)跨调用保证存活、函数入口 `RSP ≡ 8 mod 16`——dispatch 重排参(返回值绕道 callee-saved `rbx`)、`USER_ABI_RSP_OFFSET` + `static_assert` 的全部依据。
- 022 章 · [第一次跳进 Ring 3:用户态与特权隔离](022-ring3-usermode.md):`usermode_init_asm` 装配 STAR/SFMASK/EFER.SCE、`jump_to_usermode` 的 SYSRET 寄存器契约、TSS.RSP0 与 `#GP` 来源判定——本章 `syscall_init` 与它共享 STAR(两边各写一次、值一致、后者生效),`syscall_entry` 沿用 022 已开的 EFER.SCE;syscall 路径不经 TSS.RSP0,靠 `%gs:0` 自管内核栈。
- 本 tag 源码:[syscall.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/syscall.S) / [syscall.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/syscall.cpp) / [syscall.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/syscall.hpp)、[syscall_nums.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/syscall_nums.hpp) / [sys_write.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/sys_write.cpp) / [sys_exit.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/sys_exit.cpp) / [sys_yield.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/sys_yield.cpp)、[usermode.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/usermode.cpp)(GS base 页 + `wrmsr KERNEL_GS_BASE` + `_binary_hello_bin_*`)、[boot.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/boot.S)(FPU/SSE 初始化)、[process.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/process.hpp) / [process.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/process.cpp)(`fpu_state`)、[scheduler.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/scheduler.cpp)(`fxsave/fxrstor`)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp);用户态 [syscall.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/libc/syscall.cpp) / [hello.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/programs/hello.cpp) / [CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/CMakeLists.txt) / [linker.ld](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/linker.ld);测试 [test_syscall.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_syscall.cpp)(host 镜像)、[test_syscall.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_syscall.cpp)(QEMU 机内,section `Syscall Tests (023)`)。
