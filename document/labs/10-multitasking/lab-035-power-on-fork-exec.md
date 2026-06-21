---
title: Lab 035 · 通电 fork/exec
---

# Lab 035 · 通电 fork/exec

> 对应主书 [035 · 通电:让 fork/exec 真的能跑](../../book/10-multitasking/035-power-on-fork-exec.md)。承接 034(骨架搭好、尚未通电),本 lab 把 fork/exec 的每个缺口合上开关。这里给任务、给约束、给排错方向,不贴完整答案——尤其是那五堵墙,你得自己撞一遍才记得住。

## 实验目标

让 034 的 fork / execve / CoW 端到端可用:子进程从 fork 返回 0、CoW 在写时真的复制、syscall 跨进程切换正常、ELF 每页填对、内核栈溢出能被发现。

## 前置条件

- 完成 034:fork/exec/wait 的骨架、CoW 页表标记、`handle_cow_fault`(死代码)、`CpuContext`(64 字节、无 rax、无 gs)。
- 读懂 023(syscall/SYSCALL/SYSRET、`swapgs`、GS per-CPU 数据页)和 019/020(context_switch、调度器)。

## 任务分解

### 任务 1:让子进程从 fork 返回 0

034 的 fork 没给子进程设返回值。

- 在 `context_switch.S` 写 `fork_child_trampoline:xorq %rax,%rax; ret`(global)。
- fork() 里把 `child->ctx.rip` 指向它、`child->ctx.rsp` 指向 fork() 返回地址。
- **约束**:不要给 `CpuContext` 加 rax(那会牵动整套切换约定);用独立 trampoline。
- **验证**:fork 后父拿 child_pid、子拿 0,子进程能据此走不同分支。

### 任务 2:把 CoW 接进 #PF + FLAG_USER 过滤

034 的 `handle_cow_fault` 是死代码。

- 在 `exception_handlers.cpp::handle_pf` 里,当错误码 `present && write && user`(`err&0x01 && err&0x02 && err&0x04`)时调 `handle_cow_fault(fault_addr)`,成功则 `return`(不 fatal)。
- `copy_page_table_level` 加 `FLAG_USER` 过滤:`if (!(entry.raw & FLAG_USER)) continue;`(跳过内核映射);带 `FLAG_USER` 的 huge page 直接共享、不 CoW。
- **前置条件(先审查)**:所有内核硬件访问(framebuffer、AHCI、CoW 页拷贝)都走高半区 `phys + KERNEL_VMA`,不依赖 PML4[0] 恒等映射。若有谁吃恒等映射(典型:framebuffer 直接拿物理地址当虚拟地址),先改掉——否则子进程会复制到不该复制的内核大页。

### 任务 3:给 fork 保留帧指针

fork 用 RBP 定位返回地址,但 Release `-O2` 默认 `-fomit-frame-pointer`。

- 给 fork 加 `__attribute__((optimize("no-omit-frame-pointer"), noinline))`。
- **思考**:为什么必须同时 `noinline`?(内联后函数边界消失,帧指针语义失效。)
- **症状对照**:不通电时 fork 子进程会 Double Fault、异常帧 RSP 落在用户空间。

### 任务 4:调度器保存/恢复 GS MSR

MSR 是 CPU 全局寄存器,`context_switch` 不存会导致 `swapgs` 配对跨切换错乱、子进程首条 syscall 崩(RSP=0)。

- `CpuContext` 加 `gs_base`(offset 64)、`kgs_base`(offset 72),`sizeof` 64→80,加 `static_assert`。
- `context_switch.S` 用 `rdmsr`/`wrmsr` 存取 `0xC0000101`(MSR_GS_BASE)、`0xC0000102`(MSR_KERNEL_GS_BASE)。
- fork 和 TaskBuilder::build 初始化新任务:`gs_base=0`、`kgs_base=g_per_cpu.gs_page_vaddr`。
- `PerCPU` 加 `gs_page_vaddr` + `update_syscall_stack()`,每次切换刷新 `gs:0`。
- **约束**:offset 必须和 .S 里的 `64(%rdi)`/`72(%rdi)` 严格对应,用 `static_assert(offsetof(...))` 钉死。

### 任务 5:修 execve 的页内偏移

034 把段内偏移当页内偏移,第二页起 `dst+0x1000` 越界、新页全零(`.rodata` 受害)。

- 分离:`in_page_off = data_vaddr - vaddr`(页内);`seg_offset = data_vaddr - phdr.p_vaddr`(段内,算文件读位置)。写入用 `dst + in_page_off`,文件读用 `phdr.p_offset + seg_offset`。
- **诊断技巧**:加**数据内容**日志(不只字节数)——shell 写 `"\n"` 进管道变成 `"\x00"` 立刻暴露问题。

### 任务 6:栈溢出 guard page(035 半通电,诚实标注)

大对象(Terminal screen_ ~24KB + Pipe 缓冲)超 16KB 内核栈,溢出静默踩邻区。`document/notes/035/stack_guard_page_debug.md` 给出了完整修法,但**tag 035 只落地了一半**,本 lab 按实际落地范围做:

**035 已落地(做这些):**
- linker 在 `__kernel_end` 后留 64KB NOLOAD guard 区(`__boot_guard_start/end`)。
- `handle_pf` 用 `__boot_guard_start/end` + per-task `kernel_stack_guard_page` 检测,命中打 `BOOT STACK OVERFLOW DETECTED`。
- VMM 加 `split_2mb_page`(boot 栈是 2MB huge page,4KB `unmap` 拆不动,得先 split)。

**035 未落地(在报告里诚实标注,不要假装已做):**
- `#PF` 在 035 仍是 `IST=0`、GDT 没有 IST2/pf_stack(只有 `#DF` 用 IST1)。
- `split_2mb_page` 在 035 全仓无调用点(死代码),运行时的 split+unmap 没接。

**关键理解**:光有 guard 区和检测代码不够——guard 区此刻还被 2MB huge page 覆盖着(没 split+unmap),栈溢出写进去不 #PF;而且就算 #PF 了,没配 IST 的话 handler 会用溢出栈二次崩 → Double Fault → Triple Fault → 静默重启。所以这条在 035 是「骨架立了、没真正生效」的半成品。#PF 必须配 IST、guard 区必须运行时 unmap,这两步是让它真正生效的关键,留给后面。

## 接口约束

- `CpuContext` = 80 字节,`gs_base`@64、`kgs_base`@72,.S 与 .hpp offset 严格对应(`static_assert`)。
- `handle_pf` 的 CoW 分支条件:`err & 0x01 && err & 0x02 && err & 0x04`。
- `fork_child_trampoline` 必须 `global`,且只做 `xorq %rax,%rax; ret`。
- fork 的属性:`optimize("no-omit-frame-pointer")` + `noinline` 缺一不可。

## 验证步骤

**host 单测**:

```bash
ctest --test-dir build -R "fork_exec|multi_terminal|pipe" --output-on-failure
```

**QEMU 机内测试**(通电效果主要在这儿验):

```bash
cmake --build build --target run-big-kernel-test
```

预期:`run_fork_exec_tests`、`run_multi_terminal_tests` 通过,`ALL TESTS PASSED`。重点看:fork 子进程返回 0、CoW 在真 #PF 里工作、多终端不崩。

## 常见故障(就是那五堵墙)

- **fork 后 Double Fault、RSP 在用户空间**:fork 没保留帧指针,`-O2` 下 RBP 不是帧指针。加任务 3 的属性。
- **点图标 fork 后 CoW 把内核大页也复制了、行为全乱**:`copy_page_table_level` 没 `FLAG_USER` 过滤,或 framebuffer 还吃恒等映射。先做任务 2 的前置审查。
- **子进程第一条 syscall 崩、RSP=0、地址 0 缺页**:GS MSR 没跨切换保存。做任务 4。
- **shell 只回显不执行、提示符/欢迎语是 `\x00`**:execve 页内偏移 bug,`.rodata` 全零。做任务 5。
- **多终端测试静默卡死、无串口**:栈溢出 + guard page 没生效(#PF 无 IST / guard 没 unmap / huge page 拆不动)。做任务 6。

## 通过标准

- [ ] 子进程从 fork 返回 0(trampoline),父进程返回 child_pid。
- [ ] `handle_pf` 对 user-write-present 调 `handle_cow_fault` 成功返回;`copy_page_table_level` 按 `FLAG_USER` 过滤。
- [ ] fork 带帧指针属性,`-O2` 下子进程栈正确。
- [ ] `CpuContext`=80B + gs 字段,`context_switch` rdmsr/wrmsr,子进程首条 syscall 不崩。
- [ ] execve 每页填对,`.rodata` 字符串非零,shell 能执行命令。
- [ ] guard page 骨架就位(linker guard 区 + `handle_pf` 检测 + `split_2mb_page` 能力),并**诚实标注** 035 未落地的部分(`#PF` 仍 IST=0、`split_2mb_page` 无调用点、运行时未 unmap)——即 guard 在 035 尚未真正触发,是半通电。
- [ ] host + QEMU 测试通过。
