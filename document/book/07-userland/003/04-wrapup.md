---
title: 04 · 收尾:验证、下一站与参考
---

# 收尾:验证、下一站与参考

## 验证

shell 的纯逻辑(字符串工具、tokenizer、命令分发表、`cmd_echo`/`cmd_help`/`cmd_clear` 的行为、`read_line` 的退格与换行处理)在 host 上镜像着测。[test_shell.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_shell.cpp) 把 `cinux::user::strlen/strcmp/memset/memcpy/memcmp` 的边界、`tokenize`(单词/多词/首尾空白/Tab/`max_tokens` 截断/空串/纯空白)、`CmdEntry` 哨兵表的查找与未命中、三个命令的输出、`read_line` 的退格行为在 host 侧重写了一份(不链内核代码,`sys_write`/`sys_read` 用 mock,`CINUX_HOST_TEST` 门控,优化级随 CMake build type 走、默认 Debug 即 `-O0`):

```bash
ctest --test-dir build -R shell --output-on-failure
```

真正的内核基础设施(`sys_read`/`sys_write` 的 fd 与地址守卫、`SyscallNr` 常量、GDT 重排后的选择子常量、STAR 的 `0x23`/`0x10` 回读)只能在 QEMU 里验。[test_shell.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_shell.cpp) 在机内重写 tokenizer/字符串/mem 工具、直接调 `sys_write` 验守卫、断言 `SYS_read=0 / SYS_write=1 / SYS_exit=60`;而 [test_gdt_idt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_gdt_idt.cpp) 和 [test_syscall.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_syscall.cpp)、[test_usermode.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_usermode.cpp) 把「GDT 重排 + STAR 改 0x23」钉死——`GDT_KERNEL_CODE=0x10`/`GDT_USER_CODE=0x33`/`GDT_USER_DATA=0x2B`/`GDT_TSS=0x38`、`STAR[47:32]=0x10`、`STAR[63:48]=GDT_SYSRET_BASE=0x23` 三处回读。机内 test 节名是 `Shell Tests (024)`:

```bash
cmake --build build --target run-big-kernel-test
```

机内末尾打 `ALL TESTS PASSED` 就说明这套 shell 基础设施在真硬件语义下成立。要说明一句:真用户态 shell 跑在 Ring 3,机内测无法直接调它,只能验它**依赖的内核基础设施**——这是 host 单测(QEMU 之外的纯逻辑)和机内测(QEMU 里的内核侧)分工的原因。

最后是**生产现象**本身:直接跑大内核,串口应该看到 `Cinux shell - type 'help' for commands` 和 `cinux> ` 提示符;敲 `echo hello` 回车,下一行出现 `hello`;敲 `help` 回车,列出 `echo/help/clear` 三条;敲 `clear` 回车,屏幕擦净、光标归位;退格能删字。这三条命令各自行为正确、退格可用,且 SYSRETQ 出口的 CS=0x33/SS=0x2B、syscall 不再破坏用户 RBX——两颗雷都拆干净,就算 024 到位。

## 下一站

到这里,内核第一次有了一个**常驻 Ring 3、能和用户来回交互**的程序。shell 读键盘、切参数、查表派发,`sys_read` 把键盘字符一行行递上来,GDT 重排让 SYSRETQ 稳稳地穿越特权级,Console 会吃最基础的 ANSI CSI。

但你会立刻摸到 024 的天花板:这个 shell **只在内存里转**。它没有 `cat`、没有 `ls`、没有任何能碰「持久存储」的命令——因为根本没有持久存储,数据一断电就没了。shell 想读个文件,既没有文件系统,也没有哪块磁盘被接上。下一站(025,见文件系统卷)就补这块:把 AHCI/PCI 驱动接进来,让内核能和(模拟的)SATA 磁盘说上话,给数据落盘铺好物理基础。有了能读写的块设备,后面才会真正长出文件系统、以及 `cat`/`ls` 这些文件系统命令。024 的 shell 是那一切的入口——先把「人机对话」这条路走通,接下来才有意义去对话「给我看磁盘上的那个文件」。

---

### 参考

- **Intel SDM Vol.2B,SYSRET 伪代码(p.717)**(本地 `document/reference/intel/SDM-Vol2B-Instruction-Reference-M-U.pdf`):SYSRET 返回路径 `CS.Selector := IA32_STAR[63:48]+16`、`SS.Selector := (IA32_STAR[63:48]+8) OR 3; (* RPL forced to 3 *)`——用 `pdf-reader` search_pdf 核实(p.717,match `p717-match-1/3`,2026-06-21)。这是 024-01 的「对照组」:规范明确会强制 SS.RPL=3,崩溃根因是 QEMU/TCG 在 SYSRETQ 路径上没执行这一步(对 CS 执行了),故 CS 对、SS 错。
- **Intel SDM Vol.3A,SYSCALL/SYSRET 段选择子(p.184)**(本地 `document/reference/intel/SDM-Vol3A-System-Programming-Guide-Part1.pdf`):`Stack segment — IA32_STAR[63:48] + 8`(核实 p.184,match `p184-match-1`,2026-06-21)。配合 `GDT_SYSRET_BASE=0x23` 得到 SYSRETQ 目标 SS=0x2B、CS=0x33,与 `gdt.hpp` 常量一致。
- **System V AMD64 ABI**([x86-psABIs/x86-64-ABI](https://gitlab.com/x86-psABIs/x86-64-ABI),Sec. "Register Usage"):callee-saved = `rbx/rbp/r12–r15`——024-02 修复(返回值改存 `gs:16`、从 `rsp+80` 恢复用户 RBX)的依据。该清单在 019 章已按 live 核过并沿用;本次本机联网未稳定命中该页正文,标 `assumed`,TODO(reference):联网恢复后补 page anchor。
- **SYSCALL 只自动保存 RCX/R11**:Intel SDM Vol.2B,SYSCALL 伪代码(`RCX ← RIP`、`R11 ← RFLAGS`,SDM 同卷)——024-02「其余寄存器全靠软件保存」的依据。TODO(reference):本次未单独 search_pdf 命中 SYSCALL 伪代码页号,联网/预算允许时补精确页号。
- [002 章 · 让用户态会说话:SYSCALL/SYSRET 系统调用](../002/):`syscall_entry` 的 trap frame 布局与「SYSCALL 不压栈、只存 RCX/R11」的由来——本章 024-02 修复直接踩在 023 搭好的 frame(`rsp+80` = 用户 RBX)上。
- 本 tag 源码:[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/programs/shell/main.cpp) / [shell.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/programs/shell/shell.hpp) / [cmd_echo.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/programs/shell/cmd_echo.cpp) / [cmd_clear.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/programs/shell/cmd_clear.cpp) / [cmd_help.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/programs/shell/cmd_help.cpp)、[sys_read.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/sys_read.cpp)、[gdt.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/gdt.hpp) / [gdt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/gdt.cpp)、[syscall.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/syscall.cpp) / [syscall.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/syscall.S) / [usermode.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/usermode.S)、[console.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/video/console.cpp);测试 [test_shell.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_shell.cpp)(host 镜像)、[test_shell.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_shell.cpp)(QEMU 机内,节 `Shell Tests (024)`)。
