---
title: 02 · 代码路线:六处通电(fork 返回 0 / CoW 接 #PF / 帧指针 / GS MSR / 页内偏移 / 栈 guard)
---

# 代码路线:六处通电

## 子进程返回 0:fork_child_trampoline

034 的 `CpuContext` 没有 rax,fork 也没给子进程设返回值。035 没有去改上下文结构加 rax(那会牵动 019 以来的整套切换约定),而是用一个**独立的汇编入口** [context_switch.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/context_switch.S):

```asm
fork_child_trampoline:
    xorq %rax, %rax                   # rax = 0 (child sees fork() return 0)
    ret                                # return to caller
```

fork() 在造好子进程 TCB 后,把 `child->ctx.rip` 指向这个 trampoline、`child->ctx.rsp` 指向「fork() 返回地址所在的位置」。子进程首次被调度、`context_switch` 恢复它的 ctx 后跳到 trampoline,rax 被清零、`ret` 弹出返回地址——子进程就像「刚从 fork() 调用返回、返回值 0」一样继续执行。父进程则同步从 fork() 拿到 child_pid。一个 `xor` + 一个 `ret`,就把 034 留下的最大缺口填上了。

## CoW 接进 #PF,以及为什么不能照抄父进程的内核映射

034 的 `handle_cow_fault` 写好了但没人调。035 把它接进 [page_fault.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/page_fault.cpp) 的 `handle_pf`(`handle_pf` 原本写在 `exception_handlers.cpp`,后来拆分独立成 `kernel/arch/x86_64/page_fault.cpp`)——当错误码表明「页在用 + 是写」(`(err&0x01)&&(err&0x02)`),就交给 `handle_cow_fault` 处理,成功则直接返回、不算致命错误:

```cpp
// CoW fault: page is present but write-protected (fork marks shared pages
// CoW).  Resolve for ANY writer (user OR kernel): Cinux syscalls directly
// dereference user pointers (no copy_to_user yet), so the kernel legitimately
// writes CoW user pages -- e.g. waitpid storing *status into the parent's
// fork-CoW'd stack.
if ((err & 0x01) && (err & 0x02)) {
    if (cinux::proc::handle_cow_fault(fault_addr)) {
        return;
    }
}
```

注意这里**不再**检查 `(err & 0x04)`(user 位):Cinux 的 syscall 目前直接解引用用户指针(还没有 `copy_to_user`),`waitpid` 把 `*status` 写进父进程 fork-CoW 出来的栈是合法的内核写,所以 CoW 必须为任意写者解析——条件只看 present+write,`handle_cow_fault` 内部再用 `FLAG_COW` 区分真 CoW 页和只读页。

但通电时马上撞到一堵墙:034 的 `copy_page_table_level` 会把父进程 PML4[0..255] **整个**复制,包括不该复制的内核映射——1GB 的 MMIO 大页、2MB 的 RAM 大页。子进程拿到这些内核映射后,一旦写用户页触发 CoW,递归下去会去碰这些大页,行为全乱。修法是按 `FLAG_USER` **过滤**:只复制带用户位的条目,内核映射(无 `FLAG_USER`)直接跳过;遇到带 `FLAG_USER` 的 huge page,直接共享、永不 CoW(大页做 CoW 要拆页,这阶段不碰)。

```cpp
void copy_page_table_level(uint64_t src_phys, uint64_t dst_phys, int level) {
    ...
    for (uint32_t i = 0; i < PT_ENTRIES; i++) {
        if (!src_table[i].is_present()) continue;
        // 跳过内核映射:子进程通过高半区 PML4[256..511] 访问内核资源
        if (!(src_table[i].raw & FLAG_USER)) continue;
        // Huge page with FLAG_USER: share directly, never CoW.
        ...
    }
}
```

这条「按 FLAG_USER 过滤」能成立的前提是:**所有内核代码都通过高半区地址(`phys + 0xFFFFFFFF80000000`)访问硬件,不依赖 PML4[0] 的恒等映射**。审查时发现 framebuffer 驱动是个阻塞项——它原来直接把物理地址当虚拟地址用(吃恒等映射),改过来费了一番周折(见调试现场)。

## fork 的帧指针:为什么 -O2 下 RBP 不能信

fork 要让子进程从「fork() 的返回点」恢复,就得知道 fork() 的返回地址在栈上的位置。035 的 fork 用 RBP(帧指针)来定位:`[RBP+8]` 是返回地址、`[RBP]` 是调用者的 RBP。问题是:项目用 `-DCMAKE_BUILD_TYPE=Release`,编译器开 `-O2`,**默认 `-fomit-frame-pointer`**——RBP 不再是帧指针,而是被当成通用寄存器,`[RBP+8]` 根本不是返回地址。

于是 fork 读到的 `ctx.rsp` 成了垃圾值,子进程被调度进去后在一个用户空间地址上「运行」,一触发异常就 Double Fault。修法是把 fork 标记成「不内联」,并保留帧指针:

```cpp
__attribute__((noinline))
int fork(PidAllocator& pid_alloc) {
    ...
}
```

`noinline` 防止它被内联(内联后函数边界消失,帧指针语义也跟着乱)。035 当时还在函数上单独写了 `optimize("no-omit-frame-pointer")` 来强制保留帧指针;后来项目改成**全局** `-fno-omit-frame-pointer` 编译(见 `fork.cpp` 里那条注释「globally with -fno-omit-frame-pointer, so the SysV layout」),逐函数的 `optimize` 属性就不再需要,只留 `noinline`。这是个很典型的「优化与底层假设冲突」的坑——内联汇编读到的寄存器,在优化模式下含义会变。

## GS MSR 跨切换:swapgs 的配对必须跨调度保持

修完帧指针,shell 子进程能 execve 进用户态了,但**执行第一条 syscall 就崩**——Double Fault,RSP=0,还在虚拟地址 0 触发缺页。根因在 syscall 的 GS 机制。

SYSCALL 入口用 `swapgs` 切换 GS,再 `movq %gs:0, %rsp` 从 per-CPU 数据页加载内核栈。`swapgs` 是成对操作:用户态时 `MSR_GS_BASE=0`、`MSR_KERNEL_GS_BASE=per-CPU 页`;syscall 进内核后两者交换。可 **MSR 是 CPU 全局寄存器,不随任务切换自动保存**。034 的 `context_switch` 只存 callee-saved,不存 GS MSR。于是:一个 shell 在 syscall 里阻塞(已经 swapgs 过)、调度器切到别的任务、再切回来——GS MSR 状态早被搅乱,子进程再 syscall 时 `gs:0` 读到 0,RSP 变 0,崩。

修法是把 GS MSR 的状态纳入上下文。035 当时的做法是把两个 GS MSR(`MSR_GS_BASE` / `MSR_KERNEL_GS_BASE`)塞进 `CpuContext`,在 `context_switch` 里 `rdmsr`/`wrmsr` 存取。这个方向**后来被推翻了**(见本节末的演进说明),但 035 当时确实靠它让 syscall 跨切换正常。当前 `CpuContext` 的结构(`kernel/proc/cpu_context.hpp`)是:

```cpp
struct alignas(16) CpuContext {
    uint64_t r15, r14, r13, r12, rbp, rbx, rsp, rip;   // 0..56(034 就有)
    uint64_t gs_base;     // offset 64 — MSR_GS_BASE          (RESERVED)
    uint64_t kgs_base;    // offset 72 — MSR_KERNEL_GS_BASE   (RESERVED)
    uint64_t fs_base;     // offset 80 — MSR_FS_BASE          per-thread TLS
};
static_assert(sizeof(CpuContext) == 96, "CpuContext must be 96 bytes");
```

[context_switch.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/context_switch.S) 在保存/恢复 callee-saved 之外,用 `rdmsr`/`wrmsr` 读写 **`MSR_FS_BASE`(0xC0000100)**:

```asm
/* Save FS base (per-thread TLS pointer, MSR_FS_BASE) */
movq $0xC0000100, %rcx   ; rdmsr ; movl %eax,80(%rdi) ; movl %edx,84(%rdi)
/* Restore FS base */
movl 80(%rsi),%eax ; movl 84(%rsi),%edx ; movq $0xC0000100,%rcx ; wrmsr
```

> **演进说明(重要)**:035 当时把 `gs_base`/`kgs_base` 当成 per-task 字段、在切换时 `rdmsr`/`wrmsr` 存取两个 GS MSR(0xC0000101 / 0xC0000102)。后来的重构把 GS 改成 **per-CPU**:`swapgs` 规约保证内核态全程 `MSR_GS_BASE` == 本 CPU 的 PerCpu 块,**不再**跨上下文切换存取 GS MSR。于是 `gs_base`/`kgs_base` 两个字段在结构体里保留(offset 64/72 仍占位),但被标注为 `RESERVED`、切换代码不再碰它们;真正跨任务保存的 MSR 只剩 `fs_base`(offset 80,per-thread TLS,后来加的)。子进程也不再像 035 那样在 fork/TaskBuilder 里初始化 `kgs_base=g_per_cpu.gs_page_vaddr`(那个字段名后来也随 per-CPU 重构消失了)。所以读者**不要**照 035 的老办法把 GS 当 per-task 存——那是当时有效、后来被纠正的修法;现在的真理是「GS per-CPU、FS per-task」。`g_per_cpu.update_syscall_stack()` 在每次切换时刷新 `gs:0` 指向的内核栈顶。

## execve 页内偏移:为什么 .rodata 全是零

CoW 和 syscall 都通了,shell 能 execve 进用户态、能实时回显你敲的字符——但敲回车后**不执行命令**,只把你敲的那串字符原样显示,没有提示符、没有欢迎信息。排查发现:从**栈**读的数据(你敲的字符)正常,从 **`.rodata`** 读的数据(提示符串、欢迎语)全是 `\x00`。

根因在 execve 的 PT_LOAD 填充循环。034(和 035 初版)把**段内偏移**当成了**页内偏移**用:

```cpp
// 旧(有 bug)
uint64_t page_base_offset = vaddr - phdr.p_vaddr;   // 段内偏移
inode->ops->read(inode, phdr.p_offset + page_base_offset,
                 dst + page_base_offset, copy_len);  // dst + 段内偏移 → 越界!
```

第一页 `page_base_offset=0` 没事。但第二页起 `page_base_offset=0x1000`,而 `dst` 是一个**全新的 4KB 物理页**——往 `dst+0x1000` 写直接越界到相邻内存,`dst` 本身保持全零。`.text` 通常在第一页所以代码能跑;`.rodata` 在第二页及以后,所以字符串常量全是零。

修法是把「页内偏移」和「段内偏移」分开:

```cpp
uint64_t in_page_off = data_vaddr - vaddr;           // 在这一页里的偏移(第一页可能非零)
uint64_t seg_offset  = data_vaddr - phdr.p_vaddr;    // 在段里的偏移(算文件读位置用)
inode->ops->read(inode, phdr.p_offset + seg_offset,
                 dst + in_page_off, copy_len);        // 写进新页的正确位置
```

## 栈溢出 guard page:#PF 要 IST,2MB huge page 是隐形杀手

最后一堵墙最阴险:多终端测试在 QEMU 里**直接卡死、无任何串口输出**。换成堆分配就正常——典型的栈溢出。算一下对象大小:一个 `Terminal` 的 `screen_[25][80]` 缓冲约 24KB,加上几个 4KB 的 `Pipe` 缓冲,栈上轻松 ~64KB,而内核栈只有 8KB(`STACK_PAGES=2`)——溢出 8 倍。(注:`Terminal::screen_[25][80]` 是 tag-035 当时调试笔记里的对象名;后来 GUI 重写、Terminal 改走用户态 host,这名字已不在源码树,但栈溢出的定量结论不变——内核栈就是装不下几个 KB 级的栈上对象。)

更糟的是:guard page 检测代码**早就写在 `handle_pf` 里,却从来没触发过**。`document/notes/035/stack_guard_page_debug.md` 把原因扒得很细:① 注释说「guard 区已 unmap」,但 `main_test` 里**根本没有 unmap 代码**(注释撒谎);② `#PF` 在 IDT 里 `ist=0`(无独立栈),栈溢出触发 #PF 时,CPU 往已溢出的栈 push 中断帧 → 再 #PF → Double Fault → Triple Fault → QEMU 静默重启;③ boot 栈用 **2MB huge page** 映射,`VMM::unmap` 是 4KB 粒度,unmap 不了 huge page 里的单页。笔记还给出了完整修法:linker 留 64KB guard 区、GDT 加 IST2 栈、IDT 让 `#PF` 用 `IST=2`、VMM 加 `split_2mb_page`、运行时 split+unmap。

但这里**必须诚实**:这套完整修法在 tag 035 只落地了一半。真正合进 035 的是——linker 在 `__kernel_end` 后留了 64KB NOLOAD guard 区、`handle_pf` 里加了用 `__boot_guard_start/end` 和 per-task `kernel_stack_guard_page` 的检测(命中会打 `BOOT STACK OVERFLOW DETECTED`)、VMM 新增了 `split_2mb_page` 这个能力。**没**落地的是让它真正生效的运行时接线:`#PF` 在 035 仍是 `IST=0`、GDT 没有给它配独立栈(只有 `#DF` 用 IST1),而且 `split_2mb_page` 在 035 全仓**没有任何调用点**——是死代码。换句话说,guard 区此刻还被 boot 的 2MB huge page 覆盖着(没被 split+unmap),栈溢出写进去并不会 #PF,检测代码也就无从触发。这条墙在 035 是**半通电**:骨架立了,「让它真正生效的最后一接」留给了后面。把它列进通电现场,一是因为它和前几堵墙同属「让 fork/exec 跑起来」的排错脉络,二是因为它正好示范了「源码里有 ≠ 已生效」——检测代码在、guard 区在,但没接 IST、没 unmap,就是不触发。
