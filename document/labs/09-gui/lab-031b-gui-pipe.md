---
title: Lab 031b · 管道:让终端跑起真正的 shell
---

# Lab 031b · 管道:让终端跑起真正的 shell

> 这个实验对应主书 [031b · 管道:让终端跑起真正的 shell](../../book/09-gui/031b-gui-pipe.md),接在 [Lab 031](lab-031-gui-native-app.md) 之后。我们不在 lab 里贴完整答案代码——你要自己从零搭一根内核管道,把它伪装成文件挂到 shell 的 fd 0/1 上,让 031 那个本地回显终端第一次跑起真正的 Cinux shell。

## 实验目标

在 031 的终端基础上,加一套**管道 IPC**,把内核态 GUI 终端和 ring-3 用户态 shell 连成一个端到端回路:你敲 `help`,shell 执行命令,输出回显到窗口里。

要点亮的是:一根 4 KB 环形缓冲的内核管道(带半关闭与 EOF 语义)、把管道伪装成文件的 `InodeOps` 适配器、`sys_read`/`sys_write` 的派发顺序翻转、`sys_pipe` 系统调用、`FDTable::set` 把管道绑到 fd 0/1,最后在 `init.cpp` 里把这一切焊起来。这一章**还没有 fork/exec**(那是 034),所以 shell 是 `launch_first_user()` 拉起的第一个用户进程,管道是 init 在拉起 shell 之前**预先接好**的。

## 前置条件

- 跑通 Lab 031:`Terminal` 控件、`on_key` 的双分支(挂管道即转发、不本地回显)。
- 跑通 023/024:syscall 框架(`syscall_register`/`syscall_dispatch`)+ ring-3 shell,`sys_read`/`sys_write` 已存在。
- 跑通 027:VFS 的 `Inode`/`InodeOps`/`File`/`FDTable`/`g_global_fd_table()`。
- 跑通 021:`Spinlock`(及其 RAII `guard()`)。

## 任务分解

按依赖顺序,分八块做。

### 任务 1:Pipe 数据结构

新建 `kernel/ipc/pipe.{hpp,cpp}`,命名空间 `cinux::ipc`。

- 常量 `PIPE_BUFFER_SIZE = 4096`、`PIPE_SPIN_WAIT_ITERS = 1'000'000`。
- `class Pipe`:私有 `char buffer_[PIPE_BUFFER_SIZE]`、`uint32_t head_/tail_/count_`、`bool reader_open_/writer_open_`、`cinux::proc::Spinlock lock_`。构造时缓冲空、两端都 open。不可拷贝不可移动(`= delete`)。
- 想清楚**为什么用 `count_` 而不是 `head==tail` 判空满**:只有 head/tail 两个指针时,`head==tail` 既可能是空也可能是满,得浪费一格或加标志位。直接存 `count_`,`is_empty()` 看 `count_==0`、`is_full()` 看 `count_==PIPE_BUFFER_SIZE`,语义无歧义。

### 任务 2:阻塞 write / read

实现 `int64_t write(const char* data, uint64_t count)` 和 `int64_t read(char* buf, uint64_t count)`,循环到全部写完/读满。

- **write**:每轮持锁后先看 `reader_open_`,关了就返回(已写>0 返回已写字节数,否则 -1);缓冲满(`count_==PIPE_BUFFER_SIZE`)时**必须先释放锁**,再跑最多 `PIPE_SPIN_WAIT_ITERS` 次 `__asm__ volatile("sti; hlt; cli")`,每轮短暂重新取锁看是否还满/读端是否走;有空位则按 `tail_` 分两段拷贝(尾段 + 绕回头部段),用 `% PIPE_BUFFER_SIZE` 推进 `tail_`、累加 `count_`。
- **read**:对称。每轮先判 `!writer_open_ && count_==0`——这是 **EOF**,返回(已读>0 返回已读,否则 0);空了就放锁自旋;有数据按 `head_` 分两段拷贝。
- 关键两点:满了/空了**必须先放锁**再等,否则另一端拿不到锁就死锁;用 `sti;hlt` 而非纯空转,是为了让中断能进来、让另一端有机会推进。

### 任务 3:半关闭、非阻塞变体、状态查询

- **`close_reader()` / `close_writer()`**:取 `lock_.guard()` 后把对应 `bool` 置 false。注意它**不主动唤醒**任何人——靠对方在自旋循环里每轮重检这个标志位脱困。
- **`try_read` / `try_write`(非阻塞)**:`try_read` 一次性取锁,写端关且空→0(EOF),空→立即返 0(不等),有数据按 `head_` 分两段拷贝;`try_write` 读端关→-1,满→立即返 0,否则按 `tail_` 分两段拷贝。这俩是给 GUI tick 用的——tick 绝不能阻塞。
- **状态查询** `reader_alive()`/`writer_alive()`/`is_empty()`/`is_full()`/`available()`:全 `const` 且**故意不加锁**(注释写明是 lock-free 快速路径),给外部轮询者快速判断用。

### 任务 4:PipeOps——把管道端伪装成文件

新建 `kernel/ipc/pipe_ops.{hpp,cpp}`。

- **`PipeReadOps : public cinux::fs::InodeOps`**:只 override `read`,体内转发 `return pipe_->read(static_cast<char*>(buf), count)`(`offset` 形参忽略)。**不 override `write`**——继承基类 `InodeOps::write` 的默认实现(返回 -1)。
- **`PipeWriteOps : public cinux::fs::InodeOps`**:只 override `write`,转发 `pipe_->write`;`read` 继承基类返回 -1。
- 想清楚:为什么用两个子类而不是一个带方向字段的 Ops?因为方向性靠"哪个虚函数被 override"自然形成——读端写会得到 -1、写端读会得到 -1,不用查方向字段。`offset` 形参留着却不读,是为了和 `InodeOps::read/write` 签名对齐,这样管道能复用 `sys_read`/`sys_write` 的统一派发。

### 任务 5:sys_read / sys_write 派发顺序翻转

改 `kernel/syscall/sys_read.cpp` 和 `sys_write.cpp`。

- **先查 FDTable**:`g_global_fd_table().get(fd)`,若 `file && file->inode && file->inode->ops` 三者非空,就走 `file->inode->ops->read/write(file->inode, file->offset, buf, count)`,`result>0` 时推进 `file->offset`,返回 `result`。
- **只有 FDTable 无条目时**,才回退到老路径:`sys_read` 的 `fd==0` PS/2 键盘轮询、`sys_write` 的 `fd==1` 串口 `kprintf`。
- 这个顺序**必须翻转**(030 是"先 fd==0/1 再 VFS"),原因见常见故障第一条——不翻,管道永远走不到。

### 任务 6:sys_pipe 系统调用

新建 `kernel/syscall/sys_pipe.{hpp,cpp}`,并在 `kernel/syscall/syscall_nums.hpp` 加 `SYS_pipe = 22`(与 Linux x86_64 对齐)。

- **`is_user_addr(addr)`**:canonical 地址校验,拒绝 NULL 和内核态地址(bit 47 置位)。
- **`sys_pipe(pipefd_virt, ...)`**:`is_user_addr` 校验 → `new Pipe` → `new PipeReadOps(pipe)` / `new PipeWriteOps(pipe)` → 两个 `new Inode`(`ops` 挂上、`type = InodeType::Regular`)→ `table.alloc(read_inode, RDONLY)` 拿 `read_fd`、`alloc(write_inode, WRONLY)` 拿 `write_fd` → 写回用户态 `pipefd[0]=read_fd; pipefd[1]=write_fd;` → 返回 0。任一 `alloc` 失败要按序回滚(已分配的 fd `close` 掉 + `delete` 一堆对象)。
- **注册**:在 `kernel/arch/x86_64/syscall.cpp` 的 `register_builtin_handlers()` 末尾加 `syscall_register(SyscallNr::SYS_pipe, sys_pipe);`。
- 想清楚:`type` 为什么设成 `Regular` 而不是某种 "Pipe" 类型?因为派发链是 `inode->ops->read/write`,**完全不看 type**。设成 Regular 是"伪装成普通文件"的最省事做法。

### 任务 7:FDTable::set——把管道装进保留的 fd0/fd1

改 `kernel/fs/file.{hpp,cpp}` 的 `FDTable`,加 `bool set(int fd, File* file)`。

- 取 `lock_.guard()` 后,`fd` 在 `[0, FD_TABLE_SIZE)` 内就 `fds_[fd] = file` 返回 true,越界返回 false。**不释放原 File**(注释要求 caller 保证原 File 已正确释放)。
- 想清楚为什么需要它:`alloc()` 从 `FD_FIRST=3` 开始扫,**跳过保留的 0/1/2**,所以 stdin/stdout 必须用 `set` 强装,`alloc` 装不到。

### 任务 8:init.cpp 接线 + 闭合回路

改 `kernel/proc/init.cpp` 的 `kernel_init_thread`(整段在 `#ifdef CINUX_GUI` 内)。顺序很关键——**先 `gui_start()` 拿 term,再建管道,再绑 fd,再给 term 接管道,最后 `launch_first_user()`**:

- `auto* term = gui_start();`
- **stdin 管道**:`new Pipe` → `new PipeReadOps(pipe)` → `new Inode`(`ops=...`, `type=Regular`) → `new File(inode, 0, RDONLY)` → `g_global_fd_table().set(0, file)`。这是 shell 要**读**的 fd 0。
- **stdout 管道**:`new Pipe` → `new PipeWriteOps(pipe)` → `new Inode` → `new File(inode, 0, WRONLY)` → `set(1, file)`。这是 shell 要**写**的 fd 1。
- `term->set_stdin_pipe(stdin_pipe); term->set_stdout_pipe(stdout_pipe);`(接的是同一个 `Pipe*` 对象,不是 ops/inode)。
- `launch_first_user();`——shell 起来时 fd0/fd1 已就绪。
- **闭合回路**:至此 `Terminal::on_key`(挂了 stdin 管道)把按键 `try_write` 进 stdin 管道;shell `sys_read(0)` 经 FDTable→`PipeReadOps`→`Pipe::read` 拿到;shell 回显 `sys_write(1)` 经 FDTable→`PipeWriteOps`→`Pipe::write` 进 stdout 管道;`gui_tick_callback` 里 `term->poll_output()` 用 `try_read` 抽干、`write()` 落屏、`render_to_canvas` 上屏。

## 接口约束

- `PIPE_BUFFER_SIZE=4096`、`PIPE_SPIN_WAIT_ITERS=1'000'000`。
- **EOF 精确定义** = 写端关 **且** 缓冲排空(`!writer_open_ && count_==0`);`close_writer()` 后缓冲若还有数据,`read` 会先排空再返 0。`close_reader()` 后 `write` 返 -1。
- `try_read`/`try_write` 非阻塞:空返 0、满返 0(不是 -1);读端关闭 `try_write` 返 -1。
- `PipeReadOps::write` 和 `PipeWriteOps::read` **返回 -1**(继承基类默认),不是 0。
- `SYS_pipe=22`;成功返回 0(不是 fd);`pipefd[0]` 读端、`pipefd[1]` 写端;`Inode::type=Regular`。
- 方向**别装反**:fd 0 = `PipeReadOps`+`RDONLY`(shell 读 stdin),fd 1 = `PipeWriteOps`+`WRONLY`(shell 写 stdout)。

## 验证步骤

**第一步:host 单元测试**(直接链真 `pipe.cpp`+`pipe_ops.cpp`+`file.cpp`+`inode.cpp`,`test_shell_redirect` 还链 `vfs_mount.cpp`):

```bash
ctest --test-dir build -R "pipe|sys_pipe|shell_redirect" --output-on-failure
```

预期:`test_pipe`(write/read 往返、`close_reader`→write -1、`close_writer`→read 0、那条 `pipe: read drains buffer then returns 0 after close_writer` 的 drain-then-EOF、`try_write` 满返 0、`PipeReadOps` 写返 -1/`PipeWriteOps` 读返 -1)、`test_sys_pipe`(`FDTable::set`、越界拒绝、全链路往返、`sys_pipe` 拒绝 NULL 与内核态地址)、`test_shell_redirect`(用 `PipeRedirect` 夹具复刻 init 的 fd0/fd1 装配,验 `sys_read(0)`/`sys_write(1)` 经 `InodeOps` 走管道)全过。

**第二步:QEMU kernel 测试**(`main_test.cpp` 按依赖顺序注册了 `run_pipe_tests` → `run_sys_pipe_tests` → `run_window_manager_tests` → `run_terminal_tests` → `run_terminal_shell_tests`):

```bash
cmake --build build --target run-big-kernel-test
```

预期:最有分量的 `run_terminal_shell_tests()` 过——`on_key('H')` 后 stdin 管道 `try_read` 得 `'H'`、shell `try_write` 后 `poll_output` 把 `cell(0,0)` 变 `'H'`;`on_key` 把 `'\r'` 转 `'\n'`;`Terminal` 析构后 shell stdin `read` 得 0(EOF);`wm.destroy(id)` 关闭按钮触发管道关闭。QEMU 退 1 算过。

**第三步:端到端**:

```bash
cmake --build build --target run
```

预期:开机进 GUI,终端窗口里出现 Cinux shell 的真实提示符(不是 031 的纯本地回显);敲 `help` 回车,shell 的命令列表回显在窗口里;敲不认识的命令,错误信息也回到窗口。看到 shell 输出真的出现在 GUI 窗口里,整条回路就闭环了。

## 常见故障

- **接了管道,shell 输出却打到串口 / 敲键盘没反应**:`sys_read`/`sys_write` 的派发顺序没翻,还是"先 `fd==0` 键盘、`fd==1` 串口"。这样 `sys_write(1)` 会一头扎进串口、`sys_read(0)` 扎进键盘,管道被短路。必须**先查 FDTable 走 VFS/管道,再回退** fd0/fd1 老路径。
- **shell 读不到任何输入**:要么派发顺序没翻(同上),要么 fd 0/1 方向装反了(把 `PipeWriteOps` 装到了 fd 0)。
- **关窗口后 shell 卡死**:`Terminal` 析构没关管道(`close_writer`/`close_reader`),或 EOF 语义写错——以为"写端一关立刻返 0",结果缓冲里还有数据时就被当成 EOF 丢了。正确语义是"写端关 **且** 排空"才返 0。
- **GUI 一启动就冻住 / tick 卡死**:`poll_output`/`on_key` 用了**阻塞**的 `read`/`write` 而不是 `try_read`/`try_write`。tick 是事件泵,一旦阻塞整个桌面冻住。GUI 侧只能用非阻塞变体。
- **死锁**:阻塞 `write` 缓冲满时没释放锁就去等,另一端拿不到锁取不走数据。满了/空了**必须先放锁**再 `sti;hlt`。
- **`pipefd[0]`/`pipefd[1]` 写反**:约定 `pipefd[0]` 读、`pipefd[1]` 写,和 POSIX `pipe(int[2])` 一致。写反了 shell 会从写端读(返 -1)、往读端写(返 -1),什么也不通。

## 通过标准

- [ ] host `-R "pipe|sys_pipe|shell_redirect"` 全绿,`test_host` 整体不回归。
- [ ] `run-big-kernel-test` 里 `run_pipe_tests`、`run_sys_pipe_tests`、`run_terminal_shell_tests` 通过,QEMU 退 1。
- [ ] `Pipe` 4 KB 环形缓冲(`count_` 判空满)+ `Spinlock`,阻塞 `write`/`read` 满了/空了放锁自旋、两段环形拷贝;EOF(`!writer_open_ && count_==0`→read 0)、半关闭(`close_reader`→write -1)语义正确;`try_read`/`try_write` 非阻塞。
- [ ] `PipeReadOps`/`PipeWriteOps` 各只 override 一个方向,另一方向继承基类返 -1;`offset` 忽略。
- [ ] `sys_read`/`sys_write` 派发顺序翻转(先 FDTable/VFS,再回退 fd0 键盘/fd1 串口);`SYS_pipe=22` 注册并按 POSIX ABI 返回(int[2] 带出两 fd,成功返 0)。
- [ ] `FDTable::set` 能把管道装到 fd 0/1;`init.cpp` 按 `gui_start`→建两管→绑 fd0/fd1→`set_stdin/stdout_pipe`→`launch_first_user` 顺序接线。
- [ ] 端到端:终端窗口里出现 shell 提示符,敲命令能看到 shell 回显。
- [ ] 在代码或报告里**诚实标注**一条边界:这是 `sti;hlt` 有界自旋,**不是**调度器阻塞(真正的阻塞等待队列留到未来 milestone);且本 lab 没有 fork/exec,shell 是 `launch_first_user` 拉起的第一个用户进程、管道由 init 预接。不把未实现的东西写成已工作。
