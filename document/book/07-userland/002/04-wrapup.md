---
title: 04 · 收尾:验证、下一站与参考
---

# 收尾:验证、下一站与参考

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
[USER] Jumping to Ring 3: entry=0x0000000000400000 stack=0x0000000007FFFFF000
[USER] Hello from Ring 3!
[SYSCALL] sys_exit: no scheduler, halting.
```

第一行是 `launch_first_user` 报告的跳转参数,第二行就是 `hello.cpp` 通过 `sys_write` 真正打出来的(逐字节经 `kprintf` 落到串口),第三行是 `sys_exit` 在无调度器下的收尾。这三行齐了,说明「用户 `syscall` → 内核 dispatch → `sysretq` 回用户」的整条往返跑通了。

## 下一站

到这里,用户态第一次有了「跟内核说话」的嘴——`sys_write` 能把字打到屏幕上。可它的嘴只张了一下就 `exit` 了。没有 `sys_read`,用户程序听不见键盘;没有 shell,它不能常驻、不能交互地等你输命令;`sys_exit` 走的是 halt,一退整个机器就停了。

下一站([003 · shell](../003/))就补这三件事:接上 `sys_read`(真从键盘读输入)、写一个常驻的 shell(echo/help/clear 那一套)、并在 `launch_first_user` 之前启动调度器,让 `sys_exit` 走 yield、shell 能作为常驻进程一直在那儿。顺带——SYSCALL/SYSRET 这套机制一旦真用起来,会暴露两个 023 单任务时碰不到的真坑:SYSRET 的 SS RPL 问题、`syscall_entry` 里 rbx 的 clobber。那两个坑怎么定位、怎么修,是下一章的调试现场。023 把「通道」打通了,024 才有底气往这条通道上塞真东西。

---

### 参考

- **Intel SDM Vol.3A §5.8.8 "Fast System Calls in 64-Bit Mode"**(本地 `document/reference/intel/SDM-Vol3A-System-Programming-Guide-Part1.pdf`,PDF 第 184 页前后,本章 `pdf-reader` 实读核实 §5.8.8 语境与 Figure 5-14):SYSCALL 把返回地址存 `RCX`、`RFLAGS` 存 `R11`、`RIP ← IA32_LSTAR`、目标 `CS ← STAR[47:32]`、`SS ← STAR[47:32]+8`、`RFLAGS AND NOT IA32_FMASK`(故 `SFMASK=0x200` 清 IF);SYSRET 取 `STAR[63:48]+16/+8`、`RIP ← RCX`、`RFLAGS ← R11`;**SYSCALL/SYSRET 都不动 RSP**——`syscall_entry` 的换栈、恢复 `rcx/r11`、入口清 IF、`sysretq` 返回的全部依据。指令级伪码另见 Vol.2B `SYSCALL`/`SYSRET` 条目(本地 `SDM-Vol2B-Instruction-Reference-M-U.pdf`),可交叉对照。
- **Intel SDM Vol.3A · SWAPGS / IA32_KERNEL_GS_BASE**(本地同 PDF,PDF 第 99 页,本章 `pdf-reader` 实读核实):SWAPGS 交换 GS.base 与 `IA32_KERNEL_GS_BASE`(MSR `0xC0000102`),无寄存器/内存操作数——「SYSCALL 入口第一条必须 swapgs、用 `%gs:0/%gs:8` 拿 per-CPU 内核栈」的全部理由;对应 `launch_first_user` 里分配 GS 页 + `wrmsr(KERNEL_GS_BASE, ...)`。
- **Intel SDM Vol.3A · CR4.OSFXSR / FXSAVE**(本地同 PDF,本章 `pdf-reader` 实读核实):「启用 x87/SSE」步骤 1「置 `CR4.OSFXSR[bit 9]=1`」见 PDF 第 486 页、CR4 位说明见第 80 页、`OSFXSR`/`OSXMMEXCPT`/`EM`/`TS` 组合表 Table 14-1/14-2 见第 487/488 页。这里要把两类异常分开记清楚:在 `OSFXSR=0`(或 `CR0.EM=1`)下执行 SSE 指令,触发的是 invalid-opcode `#UD`(白纸黑字见 PDF 第 78 页「SSE/.../SSE4 instructions causes an invalid opcode exception (#UD)」,Table 14-1 的 `OSFXSR=0` 行也是 `#UD`);而本章「案例一」里那个 `movaps #GP` 是另一回事——SSE 的内存访问指令要求目标地址 16 字节对齐,对齐失败才 `#GP`(指令级条款见 Vol.2A `movaps` 条目)。也就是说 `OSFXSR=0` 焊的是 `#UD`,`movaps` 栈未对齐焊的是 `#GP`,两者别混。`boot.S` 置 OSFXSR/OSXMMEXCPT、清 EM/TS、`Task.fpu_state[512]` + `alignas(16)` + 调度器 `fxsave/fxrstor` 的依据也都在这一节;`FXSAVE` 保存区 512 字节、目标须 16 字节对齐的条款,另见 Vol.2A `FXSAVE` 条目(本地 `SDM-Vol2A-Instruction-Reference-A-L.pdf`)。
- **Linux man-pages · `syscall(2)`**([man7.org](https://man7.org/linux/man-pages/man2/syscall.2.html),本章 `fetchWebContent` 实读核实):架构表 x86-64 行用 `syscall` 指令、syscall 号在 `rax`、返回值在 `rax`,第二张表 arg1..arg6 = `rdi/rsi/rdx/r10/r8/r9`——**arg4 用 `r10` 而非 `rcx`(因 `rcx` 被 SYSCALL 抢去存返回地址)**的权威出处,Cinux `SyscallNr` 对齐 Linux、`syscall_entry` 把 arg4 从 `R10` 挪进 `rcx` 的依据。
- **System V AMD64 ABI**([x86-psABIs/x86-64-ABI](https://gitlab.com/x86-psABIs/x86-64-ABI),019 章已 live 核,沿用):参数寄存器顺序 `rdi/rsi/rdx/rcx/r8/r9`、第 7 参进栈、callee-saved(`rbx/rbp/r12-r15`)跨调用保证存活、函数入口 `RSP ≡ 8 mod 16`——dispatch 重排参(返回值绕道 callee-saved `rbx`)、`USER_ABI_RSP_OFFSET` + `static_assert` 的全部依据。
- [001 章 · 第一次跳进 Ring 3:用户态与特权隔离](../001/):`usermode_init_asm` 装配 STAR/SFMASK/EFER.SCE、`jump_to_usermode` 的 SYSRET 寄存器契约、TSS.RSP0 与 `#GP` 来源判定——本章 `syscall_init` 与它共享 STAR(两边各写一次、值一致、后者生效),`syscall_entry` 沿用 022 已开的 EFER.SCE;syscall 路径不经 TSS.RSP0,靠 `%gs:0` 自管内核栈。
- 本 tag 源码:[syscall.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/syscall.S) / [syscall.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/syscall.cpp) / [syscall.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/syscall.hpp)、[syscall_nums.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/syscall_nums.hpp) / [sys_write.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/sys_write.cpp) / [sys_exit.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/sys_exit.cpp) / [sys_yield.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/sys_yield.cpp)、[usermode.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/usermode.cpp)(GS base 页 + `wrmsr KERNEL_GS_BASE` + `_binary_hello_bin_*`)、[boot.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/boot.S)(FPU/SSE 初始化)、[process.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/process.hpp) / [process.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/process.cpp)(`fpu_state`)、[scheduler.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/scheduler.cpp)(`fxsave/fxrstor`)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp);用户态 [syscall.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/libc/syscall.cpp) / [hello.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/programs/hello.cpp) / [CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/CMakeLists.txt) / [linker.ld](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/linker.ld);测试 [test_syscall.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_syscall.cpp)(host 镜像)、[test_syscall.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_syscall.cpp)(QEMU 机内,section `Syscall Tests (023)`)。
