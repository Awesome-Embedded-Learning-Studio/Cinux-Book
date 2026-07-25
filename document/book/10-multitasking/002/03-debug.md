---
title: 03 · 调试现场:五堵墙
---

# 调试现场:五堵墙

这一章的「调试现场」就是上面 ②~⑥ 这五堵墙本身——它们全部来自 `document/notes/035/` 下真实记录的排错过程,每一条都是「合上开关 → 撞墙 → 定位 → 修好」。这里把五条笔记各自的**现象 → 线索 → 根因**再串一遍(①子进程返回 0 那件太干净,没有排错故事),它们合起来正是「通电 fork/exec」的完整剧情。

## 墙一:fork 后 Double Fault,RSP 跑到用户空间(fork_frame_pointer_bug)

点 shell 图标 fork 子进程,系统 `#DF`(Double Fault)卡死。异常帧里 **RSP 是个用户空间地址**,但 CPU 在内核态——内核在用用户空间栈跑,一异常就 push 到用户栈 → 再缺页 → Double Fault。线索直接指向「子进程的栈指针设错了」。根因就是上面讲的:fork 拿 RBP 当帧指针,但 `-O2` 下 RBP 不是帧指针,`ctx.rsp` 算出垃圾值。给 fork 加 `optimize("no-omit-frame-pointer")` 后,这条墙塌了。

## 墙二:fork CoW 复制了内核大页(fork_cow_huge_page_filter)

为了让 CoW 只复制用户映射(按 `FLAG_USER` 过滤),得先确保所有内核硬件访问都走高半区、不依赖 PML4[0] 恒等映射。审查下来**唯一的阻塞项是 framebuffer**:它原来直接拿物理地址 `0xFD000000` 当虚拟地址用(吃恒等映射)。改它踩了三轮:直接加 `KERNEL_VMA` 偏移 → 黑屏(高半区没映射 MMIO);用 VMM 映 4KB 页 → 画面回来但 TLB 抖动慢到没法用(~2048 个页表项);改用 2MB 大页(`map_2mb`)→ 略好但仍慢——因为映射时手贱加了 `FLAG_PCD`(缓存禁用),framebuffer 是缓存写回的,加 PCD 后每次像素写直通内存,QEMU 里极慢;**去掉 PCD** 才恢复正常。一轮「黑屏 → 慢 → 更慢 → 对了」,是硬件映射的经典折腾。

## 墙三:子进程第一条 syscall 就崩,RSP=0(syscall_gs_msr_bug)

修完帧指针,子进程能 execve 进用户态了,但**执行第一条 syscall 就 Double Fault**,RSP=0、在地址 0 触发缺页。这就是上面讲的 GS MSR 没跨切换保存:`swapgs` 的配对被调度器搅乱,子进程 syscall 时 `gs:0` 读到 0、RSP 变 0。把两个 GS MSR 纳入 `CpuContext`、`context_switch` 里 `rdmsr`/`wrmsr` 存取后,syscall 正常了。**Double Fault 且 RSP 为零或接近零,首先怀疑栈加载来源错(GS base、TSS RSP0)**——这是个很值钱的诊断直觉。

## 墙四:shell 只回显不执行,.rodata 全是零(execve_page_offset_overflow)

子进程能跑 shell 了,能实时回显按键,但回车后不执行、没有提示符。加**数据内容日志**(不是只数字节数)立刻露馅:shell 写 `"\n"` 进管道,管道里变成 `"\x00"`;从栈读的数据对、从 `.rodata` 读的全是 `\x00` 或丢失。规律太明显——`.text` 在第一页所以代码能跑,`.rodata` 在第二页及以后、内容是零 → 加载器 bug。根因就是 execve 把段内偏移当页内偏移,`dst+0x1000` 越界、新页保持全零。**调试数据损坏,区分栈和 .rodata**——栈由内核显式映射初始化,.rodata 由 execve 从 ELF 填充,加载器 bug 只影响后者。

## 墙五:多终端测试静默卡死(stack_guard_page_debug)

`test_multi_term_two_terminals_independent_pipes` 在 QEMU 里卡死、无串口输出,换堆分配就过——栈溢出(`Terminal` 的 `screen_` 缓冲 ~24KB + 几个 `Pipe` 缓冲,栈上 ~64KB,超 8KB 内核栈 8 倍)。但 guard page 检测代码早写了却不触发:注释说「已 unmap」其实没 unmap;`#PF` 没配 IST,溢出时 handler 用溢出栈二次崩;boot 栈是 2MB huge page,4KB 的 `unmap` 拆不动。笔记给出的完整修法(IST + split_2mb_page + 运行时 unmap)**在 tag 035 只落地了一半**:guard 区和检测代码进去了,但 `#PF` 仍是 IST=0、`split_2mb_page` 在 035 没有调用点——所以这条墙在 035 严格说还没彻底推倒,是「半通电」的一处。教训照样扎实:**注释说「已 unmap」不代表真 unmap;#PF 必须配 IST;2MB huge page 是隐形的 guard page 杀手**。

> 这五堵墙串起来,正好是「把 034 的 fork/exec 通电」的全部代价。034 那章我们说它是「搭好骨架、尚未通电」的半成品;035 这五条排错记录,就是「通电」两个字背后真实的血泪。每一条都不是编的,都在 `document/notes/035/` 里。
