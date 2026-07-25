---
title: 01 · 让用户态会说话:SYSCALL/SYSRET 系统调用
---

# 让用户态会说话:SYSCALL/SYSRET 系统调用

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
