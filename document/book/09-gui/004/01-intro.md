---
title: 01 · 导引:点亮什么与为什么
---

# 导引:点亮什么与为什么

> 031 的终端能打字、能渲染,但它只是在自言自语——你敲什么它画什么,没有任何东西在"听"。我们真正想要的是一个跑着 Cinux shell 的窗口:敲 `help`,shell 执行命令,把结果回显在窗口里。可这里隔着一道坎——终端跑在内核态(GUI 子系统的一部分),shell 跑在 ring 3(用户态)。两个不同特权级的东西,怎么交换字节?这一章的答案是一个 Unix 世界里用了半个世纪的抽象:**管道**。我们从零搭一根内核管道,把它伪装成文件挂到 shell 的 fd 0/1 上,让 031 那个终端第一次跑起真正的 shell。

## 这一章我们要点亮什么

开机进 GUI,终端窗口里不再是你敲什么它画什么,而是出现了 Cinux shell 的真实提示符。你敲 `help` 回车,shell 执行命令,把输出回显到同一个窗口。整条数据回路是这样闭合的:

```text
你敲 'h' → IRQ1 → ... → Terminal::on_key → stdin 管道 try_write('h')
                                              ↓
                       shell: sys_read(0) → FDTable::get(0) → inode->ops(PipeReadOps)
                                              → Pipe::read → 拿到 'h'，行编辑/回显
shell 回显: sys_write(1) → FDTable::get(1) → inode->ops(PipeWriteOps)
                                              → Pipe::write → stdout 管道缓冲
                                              ↑
            Terminal::poll_output → stdout 管道 try_read → write() → render_to_canvas → 屏幕
```

一圈走完,你敲的键变成了 shell 的输入,shell 的输出变成了窗口里的字符。这里点亮的东西不少。

核心是我们从零搭起来的一根**内核管道**:一个 4 KB 环形缓冲的单向字节流,带读写两端的半关闭语义和 EOF 约定——Unix 管道的内核实现,这一章亲手做一遍。光有管道还不够,还得让它能被 shell 的 `read`/`write` 用上,办法是**把管道伪装成文件**:把一根管道的两端各包成一个 `Inode`,挂上只读/只写的 `InodeOps`,于是已有的 `sys_read`/`sys_write` 完全不用知道"管道"这个词,照常 `inode->ops->read/write` 就能读写管道;配套的还有一个真正连接用户态的系统调用 `sys_pipe`,编号 22,和 Linux x86_64 对齐。最后,两根管道加这套文件抽象,串起了一条**跨特权级的端到端回路**——从按键到 shell 再到屏幕,跨"进程"全靠它们。这是 031 这个 milestone 的真正收尾。

## 为什么现在需要它

031 给我们的终端是一个内核态的 GUI 控件。而 shell——023 把 syscall 通到 ring 3、024 写好的那个 shell——是用户态程序。它通过 `sys_read(0)` 读输入、`sys_write(1)` 写输出。在非 GUI 模式下,fd 0 读的是 PS/2 键盘、fd 1 写的是串口,shell 直接和硬件对话。

可一旦进了 GUI,我们不再想让 shell 直接碰键盘和串口——我们想让它的输入来自那个 GUI 终端窗口,输出回到那个窗口。这就需要一个"中间人":终端把按键送给它,shell 从它读;shell 把输出送给它,终端从它读。这个中间人必须是**字节流**——shell 不知道也不关心对面是终端还是真终端还是另一个程序,它只管 `read`/`write` 字节。

而且要特别说明:这一章**还没有 fork/exec**(那是 034 的事)。所以 shell 不是被某个父进程 fork 出来再 exec 的,它是 `init.cpp` 里 `launch_first_user()` 直接拉起的第一个用户进程。它和 GUI 终端的连接,也不是靠 shell 自己调 `pipe()` 建立的,而是 `init.cpp` 在拉起 shell **之前**就把两根管道预先绑到 fd 0/1 上——shell 一开机就发现自己的 stdin/stdout 已经接好了。这个"由内核预先接线"的做法,正是没有 fork/exec 时把 GUI 和 shell 连起来的最干净的方式。

## 设计图

两根管道,方向相反,把终端和 shell 焊成一个回路:

```text
                       fd 0 (stdin)                          fd 1 (stdout)
                  ┌──────────────────┐                  ┌──────────────────┐
   Terminal       │  PipeReadOps     │       Terminal   │  PipeWriteOps    │
   (内核态 GUI)   │  ← shell 读       │                  │  shell 写 →      │
       │          └────────┬─────────┘      │           └─────────┬────────┘
       │ try_write('h')    │                │  try_read          │ sys_write(1)
       ▼                   ▼                ▼                    ▼
   ┌─────────┐      ┌──────────────┐    ┌──────────────┐     ┌─────────┐
   │ on_key  │─────▶│ stdin Pipe   │    │ stdout Pipe  │◀────│ sys_read│ ...
   │         │      │ 4KB 环形缓冲 │    │ 4KB 环形缓冲 │     │ sys_write│
   └─────────┘      └──────┬───────┘    └───────┬──────┘     └─────────┘
                           │ sys_read(0)        │ poll_output
                           ▼                    ▼
                    ┌──────────────────────────────────┐
                    │      ring-3 shell (用户态)        │
                    │  read 一字节 → 处理 → write 回显  │
                    └──────────────────────────────────┘

   同一根 Pipe 被两个 Inode 共享:
     stdin 管道: Terminal 是 writer(写按键), shell 是 reader
     stdout 管道: shell 是 writer(写输出), Terminal 是 reader
```

关键在于"伪装":每根管道的某一端被包成一个 `Inode`,挂上对应的 `PipeReadOps` 或 `PipeWriteOps`,再用 `FDTable::set` 强装到 fd 0 或 fd 1。于是 `sys_read(0)` 拿到 fd 0 的 `File`,顺着 `inode->ops` 就走到了 `PipeReadOps::read`,再走到 `Pipe::read`——shell 完全不知道自己读的是一根管道。

