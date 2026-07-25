---
title: 03 · 调试现场与收尾
---

# 调试现场与收尾

## 调试现场

这一章同样没有 notes 踩坑记录,但回路能不能通,卡点几乎都在一个地方:派发顺序。

### "接了管道,shell 输出却打到串口":派发顺序的坑

**症状。** 管道明明建好了、绑到 fd 0/1 了、终端也 `set_stdin_pipe`/`set_stdout_pipe` 了,可 shell 起来后,敲键盘没反应,shell 的输出却一条条出现在串口/QEMU 控制台上,而不是终端窗口里。

**根因。** 这正是 `sys_read`/`sys_write` 分支顺序翻转要解决的事。如果派发逻辑还停留在 030 的写法——先判 `fd==0` 走键盘、`fd==1` 走串口——那 `sys_write(1)` 会在查 FDTable **之前**就命中 `fd==1` 的老路径,直接 `kprintf` 打到串口,根本走不到管道。管道虽然接好了,却被派发顺序"短路"掉了。

**修复。** 就是前面讲的翻转:`sys_read`/`sys_write` 都改成**先查 FDTable 走 VFS/管道,再回退到 `fd==0`/`fd==1` 的老路径**。这样 `fd==1` 在 FDTable 里有管道条目时,会优先走 `inode->ops->write`(管道),只有 FDTable 无条目时才回退到串口。这个坑的教训很直接:**当一个 fd 号的语义可能被重定义时,派发绝不能死看 fd 数字,得让"有没有 VFS 条目"来决定走哪条路**。

### EOF 不是关管道的瞬间

另一个容易栽的点是 EOF 的时机。如果你以为"写端一关,读端立刻返回 0",那 shell 在管道里还有未读输出时就会把它们丢掉。实际语义是"写端关 **且** 缓冲排空"才返回 0——那条 drain-then-EOF 单测就是钉死这条:写 `'AB'` 后 `close_writer`,第一次 `read` 仍然拿到 `'AB'`(返回 2),排空后第二次才返回 0。写终端关闭逻辑时,这条保证了 shell 的最后几字节输出不会因为关窗口而截断。

## 验证

依然三层,但这一章的测试矩阵明显更厚——管道语义、syscall、端到端回路各有覆盖。

**第一层:host 单元测试。** 这一组直接链接**真** `kernel/ipc/pipe.cpp`+`pipe_ops.cpp`(+`file.cpp`/`inode.cpp`),不是 mock,在宿主上毫秒级回归管道语义:

```bash
ctest --test-dir build -R "pipe|sys_pipe|shell_redirect" --output-on-failure
```

覆盖:`test_pipe`(write/read 往返、`close_reader`→write -1、`close_writer`→read 0、drain-then-EOF、`try_write` 满返 0、`try_read` 空返 0、`PipeReadOps` 写返 -1/`PipeWriteOps` 读返 -1)、`test_sys_pipe`(`FDTable::set` 装 fd0/fd1、越界拒绝、`Pipe`+`PipeOps`+`Inode`+`File` 全链路往返、`sys_pipe` 拒绝 NULL 与内核态地址)、`test_shell_redirect`(用一个 `PipeRedirect` RAII 夹具忠实复刻 init.cpp 的 fd0/fd1 装配,验证 `sys_read(0)`/`sys_write(1)` 经 `InodeOps` 走管道的全链路)。

**第二层:QEMU kernel 测试。** 真内核对象、真堆、真 FDTable,在 QEMU 里跑。`main_test.cpp` 按依赖顺序注册了 `run_pipe_tests` → `run_sys_pipe_tests` → `run_window_manager_tests` → `run_terminal_tests` → `run_terminal_shell_tests`:

```bash
cmake --build build --target run-big-kernel-test
```

最有分量的是 `run_terminal_shell_tests`,它用纯 `Pipe` 在测试里手搓出"GUI Terminal ↔ shell"的完整往返:断言 `on_key('H')` 后 stdin 管道 `try_read` 得到 `'H'`、shell `try_write` 后 `poll_output` 把 `cell(0,0)` 变成 `'H'`;断言 `on_key` 把 `'\r'` 转成 `'\n'`(迁就 shell 行编辑);断言 `Terminal` 析构后 shell 的 stdin `read` 得到 0(EOF)、`wm.destroy(id)` 关闭按钮会触发管道关闭。退出码约定要记住:测试全过写 `exit_code=0`,经 `isa-debug-exit` 后 QEMU 退出码是 `(0<<1)|1 = 1`,所以脚本里判 `[ $QEMU_EXIT -eq 1 ]` 才算过。

**第三层:视觉效果。** 想亲眼看到 shell 提示符:

```bash
cmake --build build --target run
```

预期:开机进 GUI,终端窗口里出现 Cinux shell 的提示符(而不是 031 那种纯本地回显);敲 `help` 回车,shell 的命令列表回显在窗口里;敲一个不认识的命令,shell 的错误信息也回到窗口。这一步看到 shell 的输出真的出现在 GUI 窗口里,整条"按键 → 管道 → shell → 管道 → 屏幕"的回路就闭环了。

## 下一站

到 031b,我们有了跑着真 shell 的终端窗口。可桌面还是光秃秃的——就一个终端窗口漂在背景色上,没有图标、没有启动器、看不出这是个"桌面"。下一步很自然:给桌面画上**图标**——能点击的位图。怎么把一张位图画到画布上、怎么管理桌面上的图标,是下一章的事。

## 参考

- Linux `pipe(2)` man page(`pipefd[0]` 为读端、`pipefd[1]` 为写端,支撑本章 `sys_pipe` 的 ABI 与返回约定):https://man7.org/linux/man-pages/man2/pipe.2.html
- Linux `pipe(7)` man page(管道容量、写端关闭读端得 EOF、读端关闭写端 `EPIPE`/`SIGPIPE`,支撑本章半关闭与 EOF 语义对照——本章只对齐返回值,不实现信号):https://man7.org/linux/man-pages/man7/pipe.7.html
- Linux 内核 `arch/x86/entry/syscalls/syscall_64.tbl`(`pipe` 系统调用在 x86_64 上编号为 22,支撑本章 `SYS_pipe = 22`):https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl
- POSIX `read(2)` / `write(2)` man page(短读、写端关闭后 `read` 返回 0 表示 EOF,支撑本章 `Pipe::read` 的 EOF 语义):https://man7.org/linux/man-pages/man2/read.2.html
