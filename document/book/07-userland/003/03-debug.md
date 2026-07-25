---
title: 03 · 调试现场:PIT tick #GP 与命令集体失声
---

# 调试现场:PIT tick #GP 与命令集体失声

这一章的两颗雷都是「看起来 shell 能跑、实际埋着炸」的典型,而且都炸在 SYSCALL/SYSRET 这条用户态往返的路径上。

## 案例一:shell 起来打完 prompt,下一个 PIT tick 就 #GP(错误码 0x28)

**症状**:shell 在 Ring 3 成功起来,串口打出 `Cinux shell - type 'help' for commands` 和 `cinux> `,之后第一次 PIT 时钟中断(IRQ0)触发 IRETQ 时内核崩成 `#GP`,错误码 `0x28`。崩溃点经 `addr2line` 定位是 `irq0_stub` 的那条 `iretq`。

**根因**:`0x28` 这个错误码本身就是线索——它指向 GDT 第 5 项(User Data 段)、RPL=0。在 `pit_irq0_handler` 的 C handler 里打印中断帧的 `frame->cs`/`frame->ss`,看到进入中断时用户态的 `SS=0x0028`、而正确值应该是 `0x002B`(RPL=3);CS 倒是 `0x0033`,对的。也就是说,SYSRETQ 把用户态带回来时,给 CS 算对了、给 SS 漏了 RPL=3。SS 的 DPL/RPL 是 0、当前 CPL 却是 3,下一次 IRETQ 想把 SS 加载成 `0x28` 时,特权检查不过,炸 `#GP(0x28)`。

这里有个**必须说准**的点。Intel SDM Vol.2B 第 717 页 SYSRET 伪代码明写:返回路径上 `SS.Selector := (IA32_STAR[63:48]+8) OR 3; (* RPL forced to 3 *)`——规范是会强制 RPL=3 的。所以根因**不是**「SYSRET 不设 RPL」,而是我们这台 QEMU/TCG 在 SYSRETQ 路径上**没执行这个 `OR 3`**(对 CS 执行了,所以 CS 对;对 SS 没执行,所以 SS 错)。这是模拟器行为,不是 CPU 规范行为。

**修复**:与其赌 QEMU 会不会老实地 OR 3,不如把 RPL=3 直接编码进 STAR 基值。GDT 重排进行到一半、kernel CS 已挪到 `0x10` 时,SYSRET base 一度停在中间态 `0x20`(调试现场里 STAR 回读 `0x00200010` 就是这一刻);024-01 再把它从 `0x20` 定到 `0x23`,`usermode.S` 里写 STAR 的立即数同步成 `$0x23`。于是 `+8=0x2B`、`+16=0x33`,两个结果本身就带 RPL=3,无论模拟器要不要再 OR 一次,选择子都对。注意这个 `0x20` 只是重排途中的过渡值——023 tag 落地的 STAR 基值还是 `0x08`(那版内核 CS 在 `0x08`),并不是 `0x20`。GDT 描述符一行都不用动,因为 `0x33` 指向 idx6、`0x2B` 指向 idx5,索引没变。

**防复发**:SYSRETQ 出口的 SS.RPL 不要依赖硬件/模拟器事后补,基值自带 RPL 最稳。这和主流 x86-64 内核(如 Linux)的 STAR 写法是同一思路。诊断时,在 ISR 的 C handler 打 `frame->cs`/`frame->ss` 是区分内核/用户态中断、抓 SS 异常最快的一招;`#GP` 错误码在 IRETQ 场景下就是 CPU 试图加载的那个 selector,对着 GDT 布局一眼就能定位是哪个描述符。

## 案例二:回显正常,但 `echo hello` 打不出 hello

**症状**:shell 起来正常、提示符正常、敲键盘每个字符都能正确回显——说明 `sys_read` + `sys_write` 基本通路是通的。可是一按回车,`echo hello` 不出 `hello`,`clear` 也不清屏。所有命令集体失声。

**根因**:所有命令都失效,几乎一定不是某个命令的 bug,而是底层共性出了问题。在内核各层(syscall_dispatch / sys_read / sys_write)和 shell 主循环都加 debug 打印后,抓到一行决定性的 trace:用户明明敲了 `echo hello`(10 个字符),但 shell 拿到的 `line` 是空串、`len=1`。反汇编 `user_shell` ELF,看到编译器把 `read_line` 内联进了 `shell_main`,并把循环里的写入位置 `pos` 分配到了 RBX——核心就是这几行:

```asm
call   sys_read            ; 读一个字符
...
call   sys_write           ; 回显
lea    rdx, [rbx+0x1]      ; rdx = pos + 1
mov    BYTE PTR [rsp+rbx+0x90], al   ; line[pos] = c   ← 写到 line[rbx]
mov    rbx, rdx            ; pos = pos + 1
```

也就是说 `pos` 这个关键变量活在 RBX 里。可是每次 `sys_read`/`sys_write` 都要走 SYSCALL 进内核,而当时 [syscall.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/syscall.S) 的出口段有这么一句:

```asm
movq %rax, %rbx        # ← 用 RBX 暂存返回值!
...
addq $96, %rsp         # 释放整个 trap frame
...
movq %rbx, %rax        # 恢复返回值
```

它拿 RBX 当返回值的临时落脚点。可 RBX 是 **System V AMD64 ABI 的 callee-saved 寄存器**(和 `rbp`、`r12`–`r15` 一组),约定要求它跨函数调用保持不变。SYSCALL/SYSRET 这趟往返,从用户态角度看就是一次函数调用——返回时 RBX 必须和进去时一模一样。而 SYSCALL 指令**只**自动保存 RCX(存返回 RIP)和 R11(存 RFLAGS)两个寄存器,其余全靠软件。旧代码把返回值塞进 RBX,正好覆盖了用户的 `pos`;trap frame 里虽然压着原始 RBX(在 `rsp+80`),但出口段没去取回它,`addq $96,%rsp` 一抬手就把 frame 整个释放了,原始 RBX 彻底丢失。结果每个字符都写进 `line[1]`(RBX 被盖成返回值 1),回车时 `line[1]` 又被写成 `'\0'`,整行就成了空串——`tokenize` 切不出 `echo`,`strcmp` 自然全 miss。

**修复**:返回值换一个不会被用户态当成「承诺不变」的地方存。正好 per-CPU 的 GS scratch 页还有个空闲槽 `gs:16`(`gs:0` 存内核 RSP、`gs:8` 存用户 RSP,`gs:16` 没人用)。出口段改成:

```asm
movq %rax, %gs:16        # 返回值存进 GS scratch, 不碰 RBX
movq 0(%rsp), %rax
movq %rax, %gs:8
movq 8(%rsp), %rcx
movq 16(%rsp), %r11
movq 80(%rsp), %rbx      # ◄ 从 trap frame rsp+80 恢复用户 RBX
addq $96, %rsp
movq %gs:8, %rsp
movq %gs:16, %rax        # 取回返回值
swapgs
sysretq
```

两处改动:返回值走 `gs:16`,并在销毁 frame 之前从 `rsp+80` 把用户的 RBX 还回去。顺带一提,用户态那个 `printf` 就是这次排错逼出来的——当初想格式化数字打 debug,手写内联代码踩了空指针 `#PF`,索性把一个完整的 `printf`(支持 `%d %x %p %l %ll`)写进了 `user_libc`。

**防复发**:SYSCALL 入口/出口等同于一次函数调用,callee-saved 寄存器(`rbx/rbp/r12–r15`)必须原样归还——哪怕内核内部需要暂存,也得先存好、再恢复,绝不能拿它们当 scratch。SYSCALL 只自动管 RCX/R11,这条要刻进脑子里。怀疑寄存器被 clobber 时,反汇编用户态和内核态的入口/出口是唯一能一锤定音的手段:编译器把哪个变量分配到哪个寄存器,只有看了汇编才知道。
