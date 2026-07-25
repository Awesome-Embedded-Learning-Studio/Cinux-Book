---
title: 01 · 导引:点亮什么、为什么、设计图
---

# 导引:点亮什么、为什么、设计图

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
  fork:RBP 在 -O2 下不是帧指针 ──► ③ __attribute__((noinline)) + 全局 -fno-omit-frame-pointer
  syscall:调度器不存 GS MSR  ──► ④ CpuContext +gs_base/kgs_base(035 当时),后改 fs_base per-task
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
