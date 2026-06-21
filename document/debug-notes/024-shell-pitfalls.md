---
title: 调试档案 024 · shell 起来,SYSRETQ 的 SS 与 syscall 的 RBX 各埋一雷
tag: 024_shell
---

# 调试档案 024 · shell 起来,SYSRETQ 的 SS 与 syscall 的 RBX 各埋一雷

> 从 `document/notes/024/024-01-sysretq-ss-rpl.md`、`024-02-syscall-rbx-clobber.md` 提炼并补全「定位/防复发」,配套主书 [024 · 给内核一个能对话的用户态:shell](../book/07-userland/024-shell.md)。024 把 023 那个只打一行字就走的 hello,换成一个能读键盘、切词、分发的 REPL shell。shell 本身不难写,真正的麻烦全压在「用户态 ↔ 内核态」这条往返通道上:shell 一刻不停地 `sys_read`/`sys_write`,任何出口/入口的细节没接好,都会被高频放大成「整个 shell 报废」。这一章两个坑都典型——一个是「SYSRETQ 算出来的 SS 少了 RPL=3」,shell 刚打完 prompt、下一次 PIT 中断回来就炸 #GP;一个是「syscall 出口拿 RBX 暂存返回值」,把用户 RBX 冲掉,结果每个字符都写进同一格、命令全部失效。两条都值得记成档案。

## 案例一:shell 打完 prompt,PIT 一 tick 就炸 `#GP(0x28)`

- **症状**:shell 起来得很漂亮,串口先吐出 `Cinux shell - type 'help' for commands`,光标停在 `cinux> ` 后面等输入。可还没等我们敲键盘,下一次 PIT 时钟中断(100Hz,10ms 一次)一到,内核直接崩:

  ```
  Cinux shell - type 'help' for commands
  cinux>
  ==== EXCEPTION: #GP (vector 13) ====
    RIP   = 0xFFFFFFFF81000A3C   CS  = 0x0010
    RSP   = 0xFFFFFFFF81012F08   SS  = 0x0000
    ERROR CODE = 0x0000000000000028
  [FATAL] General Protection Fault in kernel mode (error code=0x00000000000000028)
  ```

  关键的三个特征:崩溃 RIP 落在 `irq0_stub` 的 `iretq` 上(PIT 中断返回指令);错误码是 `0x28`;而 shell 之前的 `sys_write` 明明是好的(prompt 那行字就是它打出来的),说明 SYSCALL/SYSRETQ 这条**单向**通路没毛病。也就是说,问题只在「Ring 3 → 中断进 Ring 0 → 中断回 Ring 3」这条**往返**路径上。

- **根因**:错误码 `0x28` 在 IRETQ 场景下就是 CPU 试图加载、却加载失败的那一个段选择子。`0x28` 分解出来是 GDT 第 5 项(User Data 段)、`TI=0`、`RPL=0`。中断栈帧上记录的「用户态 SS」是 `0x28`,而回到 Ring 3 时 `SS` 必须带 `RPL=3`——也就是 `0x2B`。CPU 拿 `0x28` 去给 `CPL=3` 用,DPL/RPL 检查过不去,`iretq` 当场 #GP。那这个 `0x28` 是谁放进中断栈帧的?是 SYSRETQ 把 shell 放进 Ring 3 时设的 SS。调试时我们把 `GDT_SYSRET_BASE` 还设成 `0x20`,所以 `STAR` 回读是 `STAR[63:48]=0x0020`、`STAR[47:32]=0x0010`(这一对值是「对的」),GDT[5] 描述符 dump 也是对的(access `0xF2`,DPL=3)。也就是说,问题不在我们写的常量上,而在 SYSRETQ 这个指令本身产出的 selector。当前 tag 里 `GDT_SYSRET_BASE` 已经改成了 `0x23`(见下方「修复」),回读自然变成 `0x0023`——那才是修完之后该有的样子。

  这里要特别说清楚一句,免得以后被人带沟里:Intel SDM 对 SYSRET 的描述是 `SS.Selector := (STAR[63:48] + 8) OR 3`,**规范层面它就是会 OR 上 RPL=3 的**。所以正确的归因不是「SYSRET 不设 SS.RPL」,而是「QEMU/TCG 在 SYSRETQ 路径上,给 SS 算 selector 时漏了那个 OR 3,给 CS 却做了(`CS=0x33` 是对的)。这是模拟器的行为偏差,不是 CPU 规范行为。」实机不会犯这个错,但我们在 QEMU 上跑教学内核,就得当成它会犯来防。

- **定位**:三步,每步都值得单独记。第一步,`addr2line -e build/kernel/big/big_kernel 0xFFFFFFFF81000A3C` 给出 `irq0_stub`,再反汇编确认那条指令正是 `iretq`——30 秒锁定「崩在中断返回」,而不是中断处理里某行 C 代码。第二步,分解错误码 `0x28` 到 GDT 第 5 项,知道这是「用户数据段选择子、RPL=0」,和「回到 Ring 3 需要 RPL=3」对不上,直接指向「SS 没带 RPL」。第三步是最关键的一招:在 `pit_irq0_handler` 的 C handler 里,把 `InterruptFrame` 的 `cs`/`ss` 打出来——

  ```cpp
  if ((frame->cs & 3) != 0 || dbg_cnt < 3) {
      kprintf("[DBG-PIT] tick #%d: CS=0x%04x SS=0x%04x RIP=%p\n",
              dbg_cnt++, frame->cs, frame->ss, frame->rip);
  }
  ```

  输出里前几个 tick 是 `CS=0x0010 SS=0x0018`(内核态,正常),shell 起来之后的 tick 变成 `CS=0x0033 SS=0x0028`——CS 对(CPL=3)、SS 错(RPL=0)。一行 print 把「是 SS 出问题」钉死。在 ISR 的 C handler 里打印 `frame->cs`/`frame->ss`,是区分「这是内核态中断还是用户态中断」、并快速定位 SS 异常的最直接手段。

- **修复**:思路是「别去赌 SYSRETQ 会不会 OR 3,直接把 RPL=3 编进 STAR 基值」。把 `STAR[63:48]` 从 `0x20` 改成 `0x23`,SYSRETQ 的算术就变成 `SS = 0x23 + 8 = 0x2B`(自带 RPL=3)、`CS = 0x23 + 16 = 0x33`(自带 RPL=3)。无论 CPU/QEMU 额外 OR 不 OR 3,结果都对。改动落在两处:[gdt.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/gdt.hpp) 的 `GDT_SYSRET_BASE` 从 `0x20` 改成 `0x23`,[usermode.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/usermode.S) 的 `usermode_init_asm` 里那个 STAR 立即数从 `$0x20` 改成 `$0x23`。SYSCALL 这一边不受影响,因为它读的是 `STAR[47:32]=0x10`,`CS=0x10`、`SS=0x18` 照旧。GDT 描述符一个字都不用动——`0x33` 指向 index 6、`0x2B` 指向 index 5,和之前是同一对 slot。

- **防复发**:**SYSRETQ 的 SS.RPL 在 QEMU 上不可信,基值自带 RPL 才是最稳的**。凡是涉及 SYSRET/IRETQ 回 Ring 3 的路径,都别让 selector 的 RPL 依赖 CPU 的实现细节,直接在 STAR 基值里把 `0x...3` 编进去。另一条通用经验:IRETQ 的 #GP 错误码,就是 CPU 当下试图加载的那个段 selector;结合 GDT 布局,几乎能一眼定位是哪一格描述符出了问题。再加上「ISR 的 C handler 打 `frame->cs/ss`」这一招,用户态中断的 SS 异常基本无处可藏。

## 案例二:回显正常,`echo hello` 却打不出 `hello`

- **症状**:这次 shell 起得来、prompt 也在,而且更迷惑人的是——敲键盘时字符能正常回显(`echo hello` 这十个字符一个个出现在屏幕上)。可一按回车,`echo` 该输出的 `hello` 一个字都没有,换 `clear` 也不清屏。所有命令一律失效。

  ```
  cinux> echo hello
  cinux> clear
  cinux>
  ```

  回显走的是 `sys_read` 读一个字符、`sys_write` 回显一个字符,这条路是通的;命令分发是另一条路(把整行 tokenize、比对命令名、调 handler),它全废。这种「单一功能全废、且不只一个命令中招」的形态,通常指向比某个命令 bug 更底层的东西——很可能是 syscall 机制本身、或字符串处理的内存布局出了共性毛病。

- **根因**:为了把现象看清,我们在 `syscall_dispatch`、`sys_read`、`sys_write`、shell 的 `main.cpp` 四层都加了 debug 输出。调试途中有两个副产品:一是顺手在 shell 里实现了用户态 `printf`(因为最初用内联代码格式化数字,触发了一个 `#PF at CR2=0x0` 的 NULL 解引用,索性把 `user/libc/printf.cpp` 写全了);二是发现内核 `kprintf` 不吃 `%lu`/`%lx` 这种长度修饰符,得改成 `%u`/`%x` 才正常出数字。等这两块补好,真正的异常才浮上来——debug trace 显示,用户明明敲了 `echo hello`(10 个字符),shell 拿到的却是 `line='' len=1`。

  ```
  [READ] char=0x65 'e' total=1
  [WRITE] fd=1 count=1 buf=0x7FFFFEE5F first=0x65
  e
  ...  (十个字符全部正确读入并回显)
  [DBG] line='' len=1
  ```

  字符是逐个正确读进来的,可拼回 `line` 的时候全乱了。反汇编 `user_shell` 这个 ELF,看到编译器把 `read_line` 内联进了 `shell_main`,而那个「当前写入位置 `pos`」被分配到了 `rbx`:

  ```asm
  400057: lea    rsi, [rsp+0xf]            ; &c
  400061: call   sys_read                   ; sys_read(0, &c, 1)
  ...
  40008f: call   sys_write                  ; 回显
  400099: lea    rdx, [rbx+0x1]             ; rdx = pos + 1
  40009d: mov    BYTE PTR [rsp+rbx+0x90], al ; line[pos] = c
  4000ad: mov    rbx, rdx                   ; pos = pos + 1
  ```

  每读一个字符就该 `rbx` 自增一次,十个字符后 `rbx` 该是 10。可 `sys_read`、`sys_write` 每次都走 SYSCALL 进内核。再看 `syscall.S`,它的出口段这么写:

  ```asm
  movq %rax, %rbx        ; ← 用 RBX 暂存返回值!
  movq 0(%rsp), %rax     ; user RSP
  ...
  addq $96, %rsp         ; 释放整个 trap frame
  ...
  movq %rbx, %rax        ; 恢复返回值到 RAX
  swapgs
  sysretq
  ```

  也就是说:用户 `rbx`(= `pos`)在入口被 `push` 进了 trap frame 的 `rsp+80`;可内核为了把 `rax`(返回值)挪到 `addq $96` 之后再用,拿 `rbx` 当了中转——**先覆盖了 `rbx`,后面又没从 `rsp+80` 把用户 `rbx` 恢复回来**。`addq $96, %rsp` 一执行,整个 trap frame 连同原始 `rbx` 一起被释放。SYSRETQ 回到 Ring 3 时,`rbx` 已经是 syscall 的返回值(通常 1),不是用户的 `pos` 了。

  往下追一遍执行流就更清楚:每个字符走 `sys_read` → `rbx` 被覆盖成 1 → `sys_write` → `rbx` 再被覆盖成 1 → `lea rdx,[rbx+1]` 得 `rdx=2` → `mov [rsp+rbx+0x90], al` 写进 `line[1]`(不是 `line[pos]`)→ `mov rbx,rdx` 得 `rbx=2`。下一个字符又从 `rbx=1` 开始(被下一次 syscall 覆盖回 1),又是写 `line[1]`。于是十个字符全写进 `line[1]`,前一个被后一个盖掉,`line[0]` 从头到尾没被写过。按回车时 `sys_write("\n")` 又把 `rbx` 覆盖成 1,`line[1]` 被写成 `'\0'`,最终 `line` 就是个空串,`len=1`。空串谁也匹配不上,echo/clear 自然全废。

- **定位**:在四层(syscall_dispatch / sys_read / sys_write / shell main)都加 debug 打印,先确定「读进来的字符是对的、回显是对的,坏的是拼回 line 的那一步」——具体信号就是 `line='' len=1` 与输入 `echo hello` 严重不符。然后反汇编用户态 ELF,看到 `pos` 被分配到了 `rbx`、`line[pos]` 的写入用 `rbx` 寻址。再反汇编 `syscall.S`,看到出口段 `movq %rax, %rbx` 这条用 `rbx` 暂存返回值、却没有对应的「从 `rsp+80` 恢复用户 rbx」。两边一对,根因闭合:用户态依赖 `rbx` 跨 syscall 保持不变,内核却把 `rbx` 当临时寄存器用、还破坏后没还原。反汇编是定位「寄存器被 clobber」这类问题的终极手段——编译器把哪个局部变量分配到哪个 callee-saved 寄存器,只有看了汇编才确定。

- **修复**:不再拿 `rbx` 暂存返回值,改用 per-CPU GS scratch 区一个空闲槽 `gs:16`,并在出口从 trap frame 把用户 `rbx` 原样恢复。`syscall.S` 出口段改成:

  ```asm
  movq %rax, %gs:16          ; 返回值暂存到 GS scratch slot 2
  movq 0(%rsp), %rax
  movq %rax, %gs:8           ; user RSP 存到 gs:8
  movq 8(%rsp), %rcx         ; user RIP(给 SYSRETQ 用)
  movq 16(%rsp), %r11        ; user RFLAGS(给 SYSRETQ 用)
  movq 80(%rsp), %rbx        ; ← 从 trap frame 恢复用户 RBX
  addq $96, %rsp
  movq %gs:8, %rsp           ; 切回用户栈
  movq %gs:16, %rax          ; 从 scratch 取回返回值
  swapgs
  sysretq
  ```

  `gs:16` 是安全的:GS base 那一页在 `launch_first_user()` 里分配了一整个 4KB,`gs:0` 存内核栈指针、`gs:8` 存临时 user RSP,`gs:16` 本来就空着,正好拿来当 scratch。改完之后 `echo hello` 正常出 `hello`,`clear` 正常清屏。

- **防复发**:**SYSCALL/SYSRETQ 只会自动保存/恢复 RCX 和 R11 这两个寄存器**(RCX 给 SYSRETQ 装返回 RIP、R11 给它装 RFLAGS),其余所有通用寄存器都必须由软件自己存/取。而 `rbx`、`rbp`、`r12`–`r15` 在 System V AMD64 ABI 里是 callee-saved,SYSCALL 这来回一趟在用户程序看来就是一次函数调用,这六个寄存器在进内核前后必须**逐位不变**。哪怕内核内部急着要一个临时寄存器,也只能挑那些 caller-saved 的(或者用 `gs:` scratch),绝不能动 callee-saved 那一组——动了就等于在用户不知情时改了它的局部变量。另一条更宽的经验:「所有命令都失效」这种共性故障,别逐个命令去查,先怀疑 syscall 机制本身、字符串比较、内存布局这些被所有命令共享的底层。

## 隐形的雷:single-task 时代藏着的 syscall 单栈与单分发表

上面两个坑是当场就炸的,修完 shell 就好用了。但 024 里还埋着一颗当下不炸、迟早要命的雷,单独点出来。

启动时有两步合谋出了「全局唯一 syscall 栈」:`syscall_init()` 把**当时的**内核栈指针 `rsp` 存进全局变量 `g_syscall_kernel_rsp`;紧接着 `launch_first_user()` 又把同一个 `kernel_rsp0` 写进 GS base 页的 `gs:0`(`gs_virt[0] = kernel_rsp0`)。之后每一次 SYSCALL,`syscall.S` 都是 `movq %gs:0, %rsp`——把内核栈**无脑**切到这一个固定地址。也就是说,整个内核只有**一个 syscall 栈**,所有系统调用共用它,没有任何「这是不是已经进了 syscall」的重入保护。024 是单任务内核,`launch_first_user` 只起一个 shell、shell 不返回,syscall 永远不会嵌套,这颗雷不会响。可一旦哪天 shell 跑到一半被 PIT 中断打断、而中断处理路径里又触发了系统调用(或者上了真正的多进程、两个用户进程前后脚 SYSCALL 进来),两次 syscall 会往同一块栈上压 trap frame,后一次把前一次的现场踩烂——到时候崩出来的栈帧会非常难看,而且根本看不出根因是「单栈 + 无重入门闩」。

同源的还有一个:`syscall_table` 是全局单张表,注册的 `sys_read/sys_write/sys_exit/sys_yield` 对所有用户进程一视同仁,没有「每个进程一张分发表」的概念。现在这无所谓,但等需要按进程隔离系统调用权限时,它也是要被重构的地方。

这颗雷和 019 那条「内核待错地址半区」是同一类——都是为了「先让它跑起来」而走的捷径,在单任务、单进程的世界里人畜无害,一旦上并发或多进程就会一齐反噬。眼下能做的,是在心里给它标个记号:**syscall 栈要 per-CPU/per-task 化、要有重入计数或门闩、分发表要能按进程区分**。这不是 024 该修的事,是「下一站」及以后的债务。

---

### 一句话总结

024 两个坑,一个是 **SYSRETQ 算出来的 SS 在 QEMU 上漏了 RPL=3**(`0x28` 而非 `0x2B`),修在「把 RPL=3 编进 STAR 基值 `0x23`」;一个是 **syscall 出口拿 callee-saved 的 RBX 暂存返回值**,把用户的 `pos` 冲掉、命令全废,修在「返回值改存 `gs:16`、出口从 trap frame `rsp+80` 恢复用户 RBX」。前者是「SYSRETQ 出口不可信,基值自带 RPL 最稳」,后者是「SYSCALL 只自动存 RCX/R11,callee-saved 绝不能当 scratch」。两条之外,还有一颗 single-task 时代的隐形雷:syscall 单栈、无重入门闩、分发表全局共享——当下不响,等中断里再进 syscall 或上了多进程那天,它就会爆。
