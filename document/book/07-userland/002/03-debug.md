---
title: 03 · 调试现场:movaps #GP、测试全跳过与 sys_exit halt
---

# 调试现场:movaps #GP、测试全跳过与 sys_exit halt

这一章的调试笔记里躺着三个坑,前两个是「症状误导」的典型,第三个是「设计性而非 bug」的代表。

## 案例一:用户态 movaps #GP——病因叠了两层

症状:用户态 C++ 一跑到 `movaps XMMWORD PTR [rsp], xmm0` 就 `#GP`,`RIP=0x400019`、`RSP=0x7FFFFEFD8`。第一反应是「FPU/SSE 没开」——于是去 `boot.S` 加 CR0/CR4 初始化。开了再跑,还是 `#GP`。

这里就卡住了。根因其实叠了两层:第一层确实是 FPU/SSE 没启(已修),但修完仍炸,说明还有第二层——栈不满足 SysV 对齐。`0x7FFFFEFD8` 是 `8 mod 16`,而 `movaps` 要 `0 mod 16`。修复是 `USER_ABI_RSP_OFFSET=8` + `static_assert`。教训是:**一个 `#GP` 可能同时叠了两层病因**,定位时必须分开验证——别因为「开了 FPU 还炸」就否定 FPU 那层,也别因为「FPU 是病因之一」就以为修完它就万事大吉。把对齐单独拎出来、用反汇编里 `sub rsp,0x28` 后的实际 RSP 值去对 ABI 条款,才看得清第二层。

## 案例二:加 FPU init 后,169 个大内核测试全跳过

症状:在 `boot.S` 加完 FPU 初始化,跑 `make run-kernel-test` 直接报 `Loaded ELF is not a real kernel, exiting`,169 个机内测试一个都没跑。

根因不在 FPU 逻辑本身,而在「启动指令的字节序列」。[main_test.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/test/main_test.cpp) 用大内核入口的**前 3 个字节**验真:它要求是 `FA 48 BC`(或 C7),即 `cli` + `mov rsp, imm`。原始 `boot.S` 头两条正是这个序列。可 FPU 初始化一插,变成了 `cli` + `mov %cr4,%rax`(字节 `FA 0F 20`),验真立刻判否。修复是把 FPU init 挪到「栈设置之后」,保住前两条指令的字节模式。教训是:改启动汇编要盯死那些「被外部工具当签名校验」的字节序列——你以为只是调换了下指令顺序,对校验方来说就是「整个内核不像真的了」。

## 案例三:sys_exit 走 halt 而非 yield

症状:生产 demo 跑完 `Hello from Ring 3!`,串口最后打的是 `[SYSCALL] sys_exit: no scheduler, halting.`,机器就此停住,而不是切到别的进程。

这不是 bug,是设计。本 tag 在 `launch_first_user` 之前没启动调度器,`Scheduler::is_initialized()` 返回 false,`sys_exit` 走 `cli;hlt`。那条 `yield` 分支是「为 024 留的、本 tag 跑不到」的代码。这种「跨里程碑解耦」的代价就是:syscall 模块要在「无调度器」下也能干净收场。要是图省事直接 `yield()`,在调度器没启时会崩得更难看。把双分支写明白、配上那行提示日志,是让「这是预期行为」变得可读可查。
