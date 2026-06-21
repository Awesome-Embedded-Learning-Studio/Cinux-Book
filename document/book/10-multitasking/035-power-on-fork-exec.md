---
title: 035 · 通电:让 fork/exec 真的能跑
---

# 035 · 通电:让 fork/exec 真的能跑

> 034 那一章我们搭好了 fork / execve / waitpid 的「骨架」,也诚实地说了:它是半成品——子进程还没法「返回 0」、CoW 的 `handle_cow_fault` 写好了却没接进 `#PF`、调度器不保存 GS MSR。骨架搭好不代表能跑。这一章,我们把这些全部「通电」:让 fork 真的能生出会返回 0 的子进程、让 CoW 真的在写时复制、让 syscall 在进程切换后 still 正常。通电的过程不是一帆风顺的——`document/notes/035/` 里留下了五条真实的排错记录,每一条都是「合上开关、撞墙、修好」的故事。这一章就顺着这五堵墙讲,因为它们恰好把 034 留下的每个缺口都补上了。

## 这一章我们要点亮什么

一件事最能说明问题:点桌面上的 shell 图标,内核 `fork` 出一个子进程、子进程 `execve("/bin/sh")` 成一个全新的 shell、跳进用户态跑起来——而且每个终端窗口背后是**各自独立**的 shell,互不串扰。

要让这一幕发生,034 那套「半成品」必须全部通电,一个都不能少:

- **子进程得从 fork 返回 0**。034 没接上;035 用一个汇编 trampoline 补上。
- **CoW 得在写时真的复制**。034 的 `handle_cow_fault` 是死代码;035 把它接进 `#PF`。
- **fork 得在 `-O2` 下还能正确算出子进程的栈**。这要给 fork 强制保留帧指针。
- **syscall 的 GS MSR 得跨进程切换保持配对**。调度器得保存/恢复它。
- **execve 得把 ELF 每一页都填对**。一个页内偏移的 bug 会让 `.rodata` 全是零。
- **内核栈溢出得能被发现**。否则一个大对象悄悄踩烂邻区,连个报错都没有。

这六件事里,①子进程返回 0 那件是两行汇编顺手补上的、干净利落没撞墙;其余五件(②~⑥)各撞了一堵墙,也正好对上 `document/notes/035/` 的五条笔记。

## 为什么现在需要它

034 给了我们 fork/exec/wait 的原语和 CoW 页表标记,但端到端跑不起来:你真去 fork 一个子进程、让它 execve 一个程序、跳进用户态,会立刻撞上一连串问题——子进程根本分不清自己和父进程(没有「返回 0」)、写共享页直接 fatal(CoW 没接 #PF)、子进程一执行 syscall 就崩(GS 状态错乱)。034 的测试也印证了这一点:fork 的返回语义只用字面量模拟、`handle_cow_fault` 从未被调用路径触及。

而 035b 要做的「多终端、每个终端一个独立 shell」,恰恰需要 fork+execve 端到端可用。所以 035 这一章的使命很明确:**把 034 的每个缺口都通电、撞墙、修好**,为多终端铺平内核侧的路。

## 设计图

要通电的六处,以及各自撞上的墙:

```text
  034 的半成品                          035 通电时撞的墙(notes)
  ─────────────                         ──────────────────────
  fork:子进程不返回 0      ──►  ① fork_child_trampoline(xor rax,rax;ret)
  CoW:handle_cow_fault 没接 #PF ──► ② handle_pf 调 handle_cow_fault
                                          + FLAG_USER 过滤(别复制内核大页)
  fork:RBP 在 -O2 下不是帧指针 ──► ③ __attribute__((optimize("no-omit-frame-pointer")))
  syscall:调度器不存 GS MSR  ──► ④ CpuContext +gs_base/kgs_base,context_switch rdmsr/wrmsr
  execve:页内偏移当段内偏移用 ──► ⑤ 分离 in_page_off / seg_offset(.rodata 全零)
  内核栈:溢出无检测         ──► ⑥ guard page(linker 区+检测+split_2mb_page;IST/运行时 unmap 未落地)
```

fork 子进程的「返回 0」是这样实现的——一个极简的汇编蹦床:

```text
  fork() 时:
    child->ctx.rip = &fork_child_trampoline     ← 子进程恢复时从这儿开始
    child->ctx.rsp = ... 指向 fork() 的返回地址

  fork_child_trampoline:          (context_switch.S)
    xorq %rax, %rax                ← rax = 0
    ret                            ← 弹出返回地址,「返回」到 fork() 调用点
```

子进程第一次被调度进来时,`context_switch` 跳到 `fork_child_trampoline`,它把 rax 清零再 `ret`——于是子进程「返回」到 fork 的调用点、拿到 0,正好是 Unix fork 的语义。父进程则照常从 fork() 拿到 child_pid。034 那个「子进程没法返回 0」的缺口,就这样用两行汇编合上了。

## 代码路线

### 子进程返回 0:fork_child_trampoline

034 的 `CpuContext` 没有 rax,fork 也没给子进程设返回值。035 没有去改上下文结构加 rax(那会牵动 019 以来的整套切换约定),而是用一个**独立的汇编入口** [context_switch.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/context_switch.S):

```asm
fork_child_trampoline:
    xorq %rax, %rax                   # rax = 0 (child sees fork() return 0)
    ret                                # return to caller
```

fork() 在造好子进程 TCB 后,把 `child->ctx.rip` 指向这个 trampoline、`child->ctx.rsp` 指向「fork() 返回地址所在的位置」。子进程首次被调度、`context_switch` 恢复它的 ctx 后跳到 trampoline,rax 被清零、`ret` 弹出返回地址——子进程就像「刚从 fork() 调用返回、返回值 0」一样继续执行。父进程则同步从 fork() 拿到 child_pid。一个 `xor` + 一个 `ret`,就把 034 留下的最大缺口填上了。

### CoW 接进 #PF,以及为什么不能照抄父进程的内核映射

034 的 `handle_cow_fault` 写好了但没人调。035 把它接进 [exception_handlers.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/exception_handlers.cpp) 的 `handle_pf`——当错误码表明「页在用 + 是写 + 来自用户态」(`(err&0x01)&&(err&0x02)&&(err&0x04)`),也就是「用户写了一张被 CoW 写保护的页」,就交给 `handle_cow_fault` 处理,成功则直接返回、不算致命错误:

```cpp
// CoW fault: page is present but write-protected (fork marks shared pages CoW)
if ((err & 0x01) && (err & 0x02) && (err & 0x04)) {
    if (cinux::proc::handle_cow_fault(fault_addr)) {
        return;
    }
}
```

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

### fork 的帧指针:为什么 -O2 下 RBP 不能信

fork 要让子进程从「fork() 的返回点」恢复,就得知道 fork() 的返回地址在栈上的位置。035 的 fork 用 RBP(帧指针)来定位:`[RBP+8]` 是返回地址、`[RBP]` 是调用者的 RBP。问题是:项目用 `-DCMAKE_BUILD_TYPE=Release`,编译器开 `-O2`,**默认 `-fomit-frame-pointer`**——RBP 不再是帧指针,而是被当成通用寄存器,`[RBP+8]` 根本不是返回地址。

于是 fork 读到的 `ctx.rsp` 成了垃圾值,子进程被调度进去后在一个用户空间地址上「运行」,一触发异常就 Double Fault。修法是把 fork 标记成「保留帧指针 + 不内联」:

```cpp
__attribute__((optimize("no-omit-frame-pointer"), noinline))
int fork(PidAllocator& pid_alloc) {
    ...
}
```

`optimize("no-omit-frame-pointer")` 让这一个函数保留帧指针、RBP 回归传统角色;`noinline` 防止它被内联(内联后函数边界消失,帧指针语义也跟着乱)。这是个很典型的「优化与底层假设冲突」的坑——内联汇编读到的寄存器,在优化模式下含义会变。

### GS MSR 跨切换:swapgs 的配对必须跨调度保持

修完帧指针,shell 子进程能 execve 进用户态了,但**执行第一条 syscall 就崩**——Double Fault,RSP=0,还在虚拟地址 0 触发缺页。根因在 syscall 的 GS 机制。

SYSCALL 入口用 `swapgs` 切换 GS,再 `movq %gs:0, %rsp` 从 per-CPU 数据页加载内核栈。`swapgs` 是成对操作:用户态时 `MSR_GS_BASE=0`、`MSR_KERNEL_GS_BASE=per-CPU 页`;syscall 进内核后两者交换。可 **MSR 是 CPU 全局寄存器,不随任务切换自动保存**。034 的 `context_switch` 只存 callee-saved,不存 GS MSR。于是:一个 shell 在 syscall 里阻塞(已经 swapgs 过)、调度器切到别的任务、再切回来——GS MSR 状态早被搅乱,子进程再 syscall 时 `gs:0` 读到 0,RSP 变 0,崩。

修法是把两个 GS MSR 纳入上下文。`CpuContext` 从 64 字节扩到 80 字节,加 `gs_base`(offset 64)和 `kgs_base`(offset 72):

```cpp
struct alignas(16) CpuContext {
    uint64_t r15, r14, r13, r12, rbp, rbx, rsp, rip;   // 0..56(034 就有)
    uint64_t gs_base;     // offset 64 — MSR_GS_BASE        ← 035 新增
    uint64_t kgs_base;    // offset 72 — MSR_KERNEL_GS_BASE  ← 035 新增
};
static_assert(sizeof(CpuContext) == 80, "CpuContext must be 80 bytes");
```

[context_switch.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/context_switch.S) 在保存/恢复 callee-saved 之外,用 `rdmsr`/`wrmsr` 读写这两个 MSR(0xC0000101 / 0xC0000102):

```asm
/* Save GS MSRs */
movq $0xC0000101, %rcx   ; rdmsr ; movl %eax,64(%rdi) ; movl %edx,68(%rdi)
movq $0xC0000102, %rcx   ; rdmsr ; movl %eax,72(%rdi) ; movl %edx,76(%rdi)
/* Restore GS MSRs */
movl 64(%rsi),%eax ; movl 68(%rsi),%edx ; movq $0xC0000101,%rcx ; wrmsr
movl 72(%rsi),%eax ; movl 76(%rsi),%edx ; movq $0xC0000102,%rcx ; wrmsr
```

fork 和 TaskBuilder::build 还得给新任务初始化「未交换」的 GS 状态:`gs_base=0`、`kgs_base=g_per_cpu.gs_page_vaddr`。这样子进程首次被切进来时,`wrmsr` 把 GS 设成正确的「内核态默认值」。`g_per_cpu.update_syscall_stack()` 在每次切换时刷新 `gs:0` 指向的内核栈顶。

### execve 页内偏移:为什么 .rodata 全是零

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

### 栈溢出 guard page:#PF 要 IST,2MB huge page 是隐形杀手

最后一堵墙最阴险:多终端测试在 QEMU 里**直接卡死、无任何串口输出**。换成堆分配就正常——典型的栈溢出。算一下对象大小:一个 `Terminal` 的 `screen_[25][80]` 缓冲约 24KB,加上几个 4KB 的 `Pipe` 缓冲,栈上轻松 ~64KB,而内核栈只有 16KB(`STACK_PAGES=4`)——溢出 4 倍。

更糟的是:guard page 检测代码**早就写在 `handle_pf` 里,却从来没触发过**。`document/notes/035/stack_guard_page_debug.md` 把原因扒得很细:① 注释说「guard 区已 unmap」,但 `main_test` 里**根本没有 unmap 代码**(注释撒谎);② `#PF` 在 IDT 里 `ist=0`(无独立栈),栈溢出触发 #PF 时,CPU 往已溢出的栈 push 中断帧 → 再 #PF → Double Fault → Triple Fault → QEMU 静默重启;③ boot 栈用 **2MB huge page** 映射,`VMM::unmap` 是 4KB 粒度,unmap 不了 huge page 里的单页。笔记还给出了完整修法:linker 留 64KB guard 区、GDT 加 IST2 栈、IDT 让 `#PF` 用 `IST=2`、VMM 加 `split_2mb_page`、运行时 split+unmap。

但这里**必须诚实**:这套完整修法在 tag 035 只落地了一半。真正合进 035 的是——linker 在 `__kernel_end` 后留了 64KB NOLOAD guard 区、`handle_pf` 里加了用 `__boot_guard_start/end` 和 per-task `kernel_stack_guard_page` 的检测(命中会打 `BOOT STACK OVERFLOW DETECTED`)、VMM 新增了 `split_2mb_page` 这个能力。**没**落地的是让它真正生效的运行时接线:`#PF` 在 035 仍是 `IST=0`、GDT 没有给它配独立栈(只有 `#DF` 用 IST1),而且 `split_2mb_page` 在 035 全仓**没有任何调用点**——是死代码。换句话说,guard 区此刻还被 boot 的 2MB huge page 覆盖着(没被 split+unmap),栈溢出写进去并不会 #PF,检测代码也就无从触发。这条墙在 035 是**半通电**:骨架立了,「让它真正生效的最后一接」留给了后面。把它列进通电现场,一是因为它和前几堵墙同属「让 fork/exec 跑起来」的排错脉络,二是因为它正好示范了「源码里有 ≠ 已生效」——检测代码在、guard 区在,但没接 IST、没 unmap,就是不触发。

## 调试现场

这一章的「调试现场」就是上面 ②~⑥ 这五堵墙本身——它们全部来自 `document/notes/035/` 下真实记录的排错过程,每一条都是「合上开关 → 撞墙 → 定位 → 修好」。这里把五条笔记各自的**现象 → 线索 → 根因**再串一遍(①子进程返回 0 那件太干净,没有排错故事),它们合起来正是「通电 fork/exec」的完整剧情。

### 墙一:fork 后 Double Fault,RSP 跑到用户空间(fork_frame_pointer_bug)

点 shell 图标 fork 子进程,系统 `#DF`(Double Fault)卡死。异常帧里 **RSP 是个用户空间地址**,但 CPU 在内核态——内核在用用户空间栈跑,一异常就 push 到用户栈 → 再缺页 → Double Fault。线索直接指向「子进程的栈指针设错了」。根因就是上面讲的:fork 拿 RBP 当帧指针,但 `-O2` 下 RBP 不是帧指针,`ctx.rsp` 算出垃圾值。给 fork 加 `optimize("no-omit-frame-pointer")` 后,这条墙塌了。

### 墙二:fork CoW 复制了内核大页(fork_cow_huge_page_filter)

为了让 CoW 只复制用户映射(按 `FLAG_USER` 过滤),得先确保所有内核硬件访问都走高半区、不依赖 PML4[0] 恒等映射。审查下来**唯一的阻塞项是 framebuffer**:它原来直接拿物理地址 `0xFD000000` 当虚拟地址用(吃恒等映射)。改它踩了三轮:直接加 `KERNEL_VMA` 偏移 → 黑屏(高半区没映射 MMIO);用 VMM 映 4KB 页 → 画面回来但 TLB 抖动慢到没法用(~2048 个页表项);改用 2MB 大页(`map_2mb`)→ 略好但仍慢——因为映射时手贱加了 `FLAG_PCD`(缓存禁用),framebuffer 是缓存写回的,加 PCD 后每次像素写直通内存,QEMU 里极慢;**去掉 PCD** 才恢复正常。一轮「黑屏 → 慢 → 更慢 → 对了」,是硬件映射的经典折腾。

### 墙三:子进程第一条 syscall 就崩,RSP=0(syscall_gs_msr_bug)

修完帧指针,子进程能 execve 进用户态了,但**执行第一条 syscall 就 Double Fault**,RSP=0、在地址 0 触发缺页。这就是上面讲的 GS MSR 没跨切换保存:`swapgs` 的配对被调度器搅乱,子进程 syscall 时 `gs:0` 读到 0、RSP 变 0。把两个 GS MSR 纳入 `CpuContext`、`context_switch` 里 `rdmsr`/`wrmsr` 存取后,syscall 正常了。**Double Fault 且 RSP 为零或接近零,首先怀疑栈加载来源错(GS base、TSS RSP0)**——这是个很值钱的诊断直觉。

### 墙四:shell 只回显不执行,.rodata 全是零(execve_page_offset_overflow)

子进程能跑 shell 了,能实时回显按键,但回车后不执行、没有提示符。加**数据内容日志**(不是只数字节数)立刻露馅:shell 写 `"\n"` 进管道,管道里变成 `"\x00"`;从栈读的数据对、从 `.rodata` 读的全是 `\x00` 或丢失。规律太明显——`.text` 在第一页所以代码能跑,`.rodata` 在第二页及以后、内容是零 → 加载器 bug。根因就是 execve 把段内偏移当页内偏移,`dst+0x1000` 越界、新页保持全零。**调试数据损坏,区分栈和 .rodata**——栈由内核显式映射初始化,.rodata 由 execve 从 ELF 填充,加载器 bug 只影响后者。

### 墙五:多终端测试静默卡死(stack_guard_page_debug)

`test_multi_term_two_terminals_independent_pipes` 在 QEMU 里卡死、无串口输出,换堆分配就过——栈溢出(`Terminal` 的 `screen_` 缓冲 ~24KB + 几个 `Pipe` 缓冲,栈上 ~64KB,超 16KB 内核栈 4 倍)。但 guard page 检测代码早写了却不触发:注释说「已 unmap」其实没 unmap;`#PF` 没配 IST,溢出时 handler 用溢出栈二次崩;boot 栈是 2MB huge page,4KB 的 `unmap` 拆不动。笔记给出的完整修法(IST + split_2mb_page + 运行时 unmap)**在 tag 035 只落地了一半**:guard 区和检测代码进去了,但 `#PF` 仍是 IST=0、`split_2mb_page` 在 035 没有调用点——所以这条墙在 035 严格说还没彻底推倒,是「半通电」的一处。教训照样扎实:**注释说「已 unmap」不代表真 unmap;#PF 必须配 IST;2MB huge page 是隐形的 guard page 杀手**。

> 这五堵墙串起来,正好是「把 034 的 fork/exec 通电」的全部代价。034 那章我们说它是「搭好骨架、尚未通电」的半成品;035 这五条排错记录,就是「通电」两个字背后真实的血泪。每一条都不是编的,都在 `document/notes/035/` 里。

## 验证

**第一层:host 单元测试。** 多终端、fork/exec、pipe 的纯逻辑(host 镜像):

```bash
ctest --test-dir build -R "multi_terminal|fork_exec|pipe" --output-on-failure
```

**第二层:QEMU kernel 测试。** 真跑内核、走真 fork/exec/#PF:

```bash
cmake --build build --target run-big-kernel-test
```

覆盖 `run_fork_exec_tests`、`run_multi_terminal_tests` 等。这一层对 035 尤其重要——CoW 是否真在 #PF 里工作、子进程是否真返回 0、GS MSR 是否跨切换保持,都得在真 CPU + 真 syscall 上验。

**第三层:端到端。** 035 不只是内核基础设施——同一个 tag 里,GUI 侧已经把它用起来了:[gui_init.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/gui/gui_init.cpp) 的 `create_shell_terminal()` 会在点 shell 图标时 `fork` + `execve("/bin/sh")`,把一个独立 shell 跑进用户态:

```bash
cmake --build build --target run
```

点 shell 图标,你会看到 fork+execve 出一个独立 shell、进用户态跑起来、能执行命令——这就是 035 这几堵墙被推倒后,内核侧真正能端到端跑的证据。至于「开多个终端、各自独立 shell」的完整体验(私有管道、私有 fd 表),是 035b 的主题,下一章展开。

## 下一站

到 035,内核侧的 fork/exec 彻底通了:子进程会返回 0、CoW 在写时真的复制、syscall 跨切换正常、ELF 每页填对。地基夯实了——而且同一个 tag 里,这套能力就已经被 GUI 侧的 `create_shell_terminal` 用了起来(fork+execve 出独立 shell)。

下一章(035b),我们专门看 GUI 那半:怎么把「点图标 → fork+execve → 独立 shell」做成**多终端**——每个终端一对私有管道、一张私有 fd 表,多个 shell 进程互不串扰。那是整条 GUI/多任务弧的高潮。

## 参考

- Intel SDM / AMD64 APM:`swapgs` 指令、`MSR_GS_BASE`(0xC0000101)/`MSR_KERNEL_GS_BASE`(0xC0000102)、SYSCALL/SYSRET、TSS 的 IST(Interrupt Stack Table)、页表 available bits。这些是 GS MSR、IST、CoW 那几堵墙的硬件依据。
- Linux man-pages `fork(2)`:fork 在父进程返回子 PID、在子进程返回 0——035 用 `fork_child_trampoline` 终于实现。https://man7.org/linux/man-pages/man2/fork.2.html
- OSDev Wiki — Page Fault:error code 的 bit0(present)/bit1(write)/bit2(user)含义,`handle_pf` 据此分流 demand-paging / CoW / 致命。https://wiki.osdev.org/Exceptions#Page_Fault
- OSDev Wiki — ELF:`PT_LOAD` 段、`p_filesz`/`p_memsz`,execve 页内偏移 bug 的背景。https://wiki.osdev.org/ELF
