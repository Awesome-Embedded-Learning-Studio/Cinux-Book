---
title: 04 · 收尾:验证、下一站与参考
---

# 收尾:验证、下一站与参考

## 验证

先在 host 上把纯算术的部分钉死。`ctest -R usermode` 跑 [test_usermode.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_usermode.cpp)(`-DCINUX_HOST_TEST` 门控,不链内核代码),它镜像验证:用户态常量(`USER_ENTRY_BASE=0x400000`、`USER_STACK_TOP=0x7FFFFF000`、`USER_STACK_PAGES=4`)、栈大小与基址(16 KB、`0x7FFFFB000`)、字节码(`cli=0xFA`、`hlt=0xF4`、`jmp -4` 是 `EB FC`)、STAR 计算(`SYSRET CS = 0x08+16+3 = 0x1B`、SS 推出 `0x13`、高 32 位 = `0x00080008`)、RFLAGS `0x202`、用户页标志 `0x7`、镜像 `TestTSS` 的布局(`sizeof=104`、`ist@36`、`iomap_base@102`、`rsp@4`)、以及 `InterruptFrame` 用 `(cs & 0x03)` 判用户态:

```bash
ctest --test-dir build -R usermode --output-on-failure
```

真正的 `SYSRET`/真 GDT/真 IDT/真 PMM/VMM/`AddressSpace`,只能在 QEMU 里验。`run-big-kernel-test` 跑 [test_usermode.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_usermode.cpp),test section 名是 `Usermode Tests (022)`,八组:TSS RSP0 读写、STAR/EFER MSR 读回(`STAR[63:48]=0x08`、`EFER.SCE=1`、SFMASK 仅验「写 `0x200` 不 `#GP`」)、用户 `AddressSpace` 创建/map/translate/隔离、段选择子 inline asm(`mov %cs/%ds/%ss`、`str`)、常量、字节码、IST1 配置、`usermode_init` 已调用:

```bash
cmake --build build --target run-big-kernel-test
```

最后是**生产 demo** 的现象:直接跑大内核,串口应该依次看到 `[USER] Setting up first user-mode program...` → `[USER] User address space activated (PML4 at phys 0x...)` → `[USER] Jumping to Ring 3: entry=0x0000000000400000 stack=0x0000000007FFFFF000` → 异常转储里 `CS = 0x001b` → `[EXCEPTION] #GP at RIP=0x... from user mode (Ring 3)` → `Privileged instruction executed in Ring 3 -- protection works!`,然后机器 halt 住。`CS=0x1B`(用户代码段,RPL=3)加上那句 `protection works`,就是本章交付的全部证据。

## 下一站

到这里,我们第一次让 CPU 跑进了 Ring 3,又第一次让一条特权指令撞墙弹回来。隔离的「墙」立起来了——但墙是单面通的:用户态进得去,却没法和内核「说话」。它只能用触发异常这种粗暴的方式引起注意,然后整个机器就停了。

真实的用户态不该这样。用户程序应该能**合法地**请求内核替它做事——往屏幕上写一行字、退出自己、让出 CPU——而不是只能 `cli` 撞墙。这就需要一条从 Ring 3 回到 Ring 0 的「正门」:`SYSCALL` 指令,以及内核里处理它的一套入口。下一站([002 · SYSCALL/SYSRET 系统调用](../002/))就接这件事:装 syscall 入口、写 `sys_write`/`sys_exit`/`sys_yield` 这几个最基础的系统调用、搭一个最小的 user libc、再让一个真正的 ELF 用户程序(`hello`)跑起来。022 这章立起来的「墙」和那个硬编码的四字节,是那一切的起点——得先证明用户态被关在笼子里,后面给它开一扇合法的门才有意义。

---

### 参考

- **Intel SDM Vol.3A §5.8.8 "Fast System Calls in 64-Bit Mode"**(本地 `document/reference/intel/SDM-Vol3A-System-Programming-Guide-Part1.pdf`,手册 5-22 页 / PDF 第 184 页,本章 `pdf-reader` 实读核实):`SYSRET`(REX.W/64 位用户)的 `CS ← IA32_STAR[63:48]+16`、`SS ← IA32_STAR[63:48]+8`、`RIP ← RCX`、`RFLAGS ← R11`,以及 SYSRET 不修改 RSP、SYSCALL 方向的 `CS ← STAR[47:32]`、`RFLAGS AND NOT IA32_FMASK`。`jump_to_usermode` 的寄存器契约和「软件必须自己切 RSP」的全部依据。
- **Intel SDM Vol.3A §5.8.8 Figure 5-14 "MSRs Used by SYSCALL and SYSRET"**(同 PDF,目录第 36 页列出 Figure 5-14 在 5-23 页):`IA32_STAR` 的 `[63:48]`/`[47:32]` 位域布局、`IA32_FMASK` 仅作用于 SYSCALL 方向——本章 STAR 装配与「SFMASK 写不写无所谓」论断的硬件出处。
- **Intel SDM Vol.3A §5.9 "Privileged Instructions"**(同 PDF,手册 5-23 页起):`HLT`/`WRMSR`/`RDMSR`/`CLTS`/`LTR` 等列名,CPL 非 0 执行即 `#GP`——本章用户字节流 `0xFA(cli)`/`0xF4(hlt)` 在 Ring 3 触发 `#GP`、反向证明隔离的依据。(注:`CLI` 受 `CPL <= IOPL` 约束、用户态 IOPL 通常为 0,故 Ring 3 执行 `cli` 同样 `#GP`;此条 Vol.2A `CLI` 条目,引用时挂 SDM 章节。)
- **Intel SDM Vol.3A §8 Task Management · 64-Bit TSS**(同 PDF,§8.7 Task Management in 64-Bit Mode,Figure 8-11 64-Bit TSS Format):104 字节 TSS 的规范出处,`RSP0`(`rsp[0]`)与 `IST1`(`ist[0]`)字段。本章 `static_assert(sizeof(TaskStateSegment)==104)` 与 host 测试的 `TestTSS` 偏移校验即据此;字段偏移以代码与测试为准,不引死 Figure 编号。
- **Intel SDM Vol.2D `WRMSR` 条目**(本地 `document/reference/intel/SDM-Vol2D-Instruction-Reference-W-Z.pdf`,Chapter 6,手册 6-9 页起):`WRMSR` 把 `EDX:EAX` 写入 `MSR[ECX]`(高 32 位 ← EDX、低 32 位 ← EAX),并对 `RAX`/`RDX` 的高 32 位不予理会——调试现场案一 bug 二(`shlq $32` vs `shlq $16`)的根因依据。
- 018 章 · 地址空间:`AddressSpace` 的「内核半区 `PML4[256..511]` 共享、用户半区私有」设计——`launch_first_user` 为什么要手动把 framebuffer 的 identity-mapping 复制进用户 PDPT,就是对上这个设计。
- 本 tag 源码:[usermode.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/usermode.S) / [usermode.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/usermode.cpp) / [usermode.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/usermode.hpp)、[gdt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/gdt.cpp) / [gdt.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/gdt.hpp)、[idt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/idt.cpp)、[exception_handlers.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/exception_handlers.cpp)(`handle_gp`)、[vmm.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/vmm.cpp)(`walk_level` 的 `user_flag`)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp);测试 [test_usermode.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_usermode.cpp)(host 镜像)、[test_usermode.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_usermode.cpp)(QEMU 机内,section `Usermode Tests (022)`)。
