---
title: 02 · Pipe 实现:环形缓冲到系统调用
---

# Pipe 实现:环形缓冲到系统调用

## 代码路线

### Pipe:4KB 环形缓冲,为什么用 count_ 而非 head==tail

管道的核心在 [pipe.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/ipc/pipe.hpp) / [pipe.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/ipc/pipe.cpp)。一根管道的全部状态是:

```cpp
static constexpr uint32_t PIPE_BUFFER_SIZE    = 4096;       // 一页
static constexpr uint32_t PIPE_SPIN_WAIT_ITERS = 1'000'000; // 满/空时最大自旋次数

class Pipe {
    char     buffer_[PIPE_BUFFER_SIZE];
    uint32_t head_;          // 下一个要读的字节
    uint32_t tail_;          // 下一个要写的空位
    uint32_t count_;         // 当前缓冲里的字节数 (0 .. PIPE_BUFFER_SIZE)
    bool     reader_open_;
    bool     writer_open_;
    cinux::proc::Spinlock lock_;   // 保护上面所有可变状态
};
```

这里有个环形缓冲的经典选择题:`head_`/`tail_`/`count_` 三元组,而不是只用 `head`/`tail` 两个指针。后者有个老问题——`head == tail` 既可能是"空"也可能是"满",要么浪费一格,要么得加个标志位。Cinux 直接多存一个 `count_`:`is_empty()` 看 `count_==0`、`is_full()` 看 `count_==PIPE_BUFFER_SIZE`,语义无歧义,`head_`/`tail_` 只管位置。代价是多一个 `uint32_t`,对 4 KB 缓冲完全可以接受。两个 `bool` 记录读写端是否还开着——这是半关闭和 EOF 的基础。

### write / read:sti;hlt 有界自旋与让出锁

`Pipe::write` 要处理"缓冲满了怎么办"。它的做法是**有界自旋**,而不是无限等待:

```cpp
int64_t Pipe::write(const char* data, uint64_t count) {
    if (data == nullptr || count == 0) return -1;
    uint64_t written = 0;
    while (written < count) {
        lock_.acquire();
        if (!reader_open_) {                       // 读端关了,写没意义
            lock_.release();
            return (written > 0) ? written : -1;
        }
        if (count_ == PIPE_BUFFER_SIZE) {          // 满了
            lock_.release();                       // 必须先放锁,否则读端永远进不来 → 死锁
            for (uint32_t i = 0; i < PIPE_SPIN_WAIT_ITERS; i++) {
                __asm__ volatile("sti; hlt; cli"); // 开中断 + 暂停,让读端有机会跑
                lock_.acquire();
                bool still_full  = (count_ == PIPE_BUFFER_SIZE);
                bool reader_gone = !reader_open_;
                lock_.release();
                if (!still_full || reader_gone) break;
            }
            continue;
        }
        /* 有空位:按 tail_ 分两段拷贝(尾段 + 绕回头部段),推进 tail_/count_ */
        ...
    }
    return written;
}
```

这里有两个要点。第一,**满了必须先释放锁再去等**。如果持着锁死等读端来取数据,可读端要取数据也得拿这把锁——死锁。所以"等待"这一步必须把锁放掉。第二,**用 `sti; hlt; cli` 而不是纯空转**。`hlt` 让 CPU 暂停省电,`sti` 临时开中断是为了让中断(包括另一个上下文里读端/写端的推进)能进来——这正是这根管道能跑通而不死锁的物理基础。

`Pipe::read` 完全对称:空了就放锁、`sti;hlt` 等写端送数据。EOF 的判定在循环开头:

```cpp
if (!writer_open_ && count_ == 0) {     // 写端关了 且 缓冲已排空
    lock_.release();
    return (total_read > 0) ? total_read : 0;   // EOF = 返回 0
}
```

要诚实说明:这是 `sti;hlt` 的**有界自旋**,不是真正的调度器阻塞。源码头文件注释写得很直白——`True scheduler-based blocking will be added in a future milestone`。在还没有把管道接进调度器等待队列的现在,沿用既有的 `sys_read` 自旋模式是个够用的折中。它之所以能工作,是因为读写两端跑在不同的执行上下文(一个是 ring-3 shell 经 syscall 进来的阻塞 `read`,一个是 GUI tick 里的非阻塞 `try_read`),`sti;hlt` 让出的 CPU 时间足够让对方推进。

### 半关闭与 EOF:close 不是立刻返回 0

管道有两端,关一端叫"半关闭"。`close_writer()` / `close_reader()` 只是翻一个 `bool`,并不主动唤醒谁——靠对方在自旋循环里每轮重新检查这个标志位脱困:

```cpp
void Pipe::close_writer() { auto guard = lock_.guard(); writer_open_ = false; }
void Pipe::close_reader() { auto guard = lock_.guard(); reader_open_ = false; }
```

EOF 的精确定义是**"写端关闭 且 缓冲已排空"**,不是"写端关闭"。这条很容易记错。写端 `close_writer()` 之后,如果缓冲里还有数据,`read` 会**先把残余数据读出来**,直到排空才返回 0。为什么要这样?因为像 shell 输出的最后几个字节不该因为关管道就丢掉。对应到那条 `pipe: read drains buffer then returns 0 after close_writer` 单测:写 `'AB'` 后 `close_writer()`,第一次 `read` 拿到 `'AB'`(返回 2),第二次 `read` 才返回 0(EOF)。

反方向:读端关闭后,`write` 返回 `-1`(对应 Unix 的 `EPIPE`/`SIGPIPE` 的返回值语义——这一章不实现信号,只对齐返回值)。这就是为什么 `Terminal` 的析构函数里,关 stdin 写端会让 shell 的 `read` 得到 0(EOF)、关 stdout 读端会让 shell 的 `write` 得到 -1:

```cpp
Terminal::~Terminal() {
    if (stdin_pipe_  != nullptr) { stdin_pipe_->close_writer();  stdin_pipe_  = nullptr; }
    if (stdout_pipe_ != nullptr) { stdout_pipe_->close_reader(); stdout_pipe_ = nullptr; }
}
```

终端窗口被关掉时,shell 能据此干净退出,而不是永远卡在 `read`/`write` 上。

### try_read / try_write:为什么 GUI tick 绝不能阻塞

`write`/`read` 是会自旋阻塞的。可 GUI 终端在 PIT tick 回调里轮询 shell 输出,那是整个 GUI 的事件泵——**一旦它阻塞,整个桌面就冻住**。所以管道还提供了非阻塞的 `try_read`/`try_write`,空了/满了立刻返回 0,绝不等待:

```cpp
int64_t Pipe::try_read(char* buf, uint64_t count) {
    auto guard = lock_.guard();
    if (!writer_open_ && count_ == 0) return 0;   // 写端关且空 = EOF
    if (count_ == 0) return 0;                    // 空 = 立刻返回,不等
    /* 按 head_ 分两段拷贝 min(count, count_) 字节 */
}
```

`Terminal::poll_output()` 就靠它把 shell 这一拍的输出一次性抽干:

```cpp
void Terminal::poll_output() {
    if (stdout_pipe_ == nullptr) return;
    char buf[256];
    while (true) {
        int64_t n = stdout_pipe_->try_read(buf, sizeof(buf));
        if (n <= 0) break;            // 没数据(0)或出错(-1)都停
        write(buf, static_cast<uint64_t>(n));   // 灌进字符缓冲(write 里已含 ANSI/换行)
    }
}
```

`try_write` 同理:终端把按键写进 stdin 管道时,如果 shell 没在读导致缓冲满,`try_write` 返回 0 而不是把内核挂死在自旋里。阻塞版给 shell(它可以等),非阻塞版给 GUI(它不能等)——这套不对称是刻意的。

还有一组只读的状态查询 `reader_alive()`/`writer_alive()`/`is_empty()`/`is_full()`/`available()`,全部 `const` 且**故意不加锁**(注释写明是 lock-free 的快速路径)。终端每帧想"还有没有数据可渲染",不必每次都去抢 `Spinlock`,锁自由读一个 `count_`/标志位即可,最坏只是读到稍旧的值,不影响正确性。

### PipeOps:把管道端伪装成文件

到这里管道还是个裸对象。要让它走 `sys_read`/`sys_write` 的既有路径,得把它伪装成文件——也就是包成一个 `Inode`、挂上一套 `InodeOps`。这就是 [pipe_ops.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/ipc/pipe_ops.hpp) 干的事:

```cpp
class PipeReadOps : public cinux::fs::InodeOps {   // 读端:只 override read
public:
    explicit PipeReadOps(Pipe* pipe);
    int64_t read(const Inode* inode, uint64_t offset, void* buf, uint64_t count) override {
        return pipe_->read(static_cast<char*>(buf), count);   // offset 忽略
    }
private:
    Pipe* pipe_;
};

class PipeWriteOps : public cinux::fs::InodeOps {  // 写端:只 override write
public:
    int64_t write(Inode* inode, uint64_t offset, const void* buf, uint64_t count) override {
        return pipe_->write(static_cast<const char*>(buf), count);
    }
    ...
};
```

这个设计有两个巧处。一是**用两个独立的子类,而不是一个带方向字段的 Ops**。`PipeReadOps` 只 override `read`、`PipeWriteOps` 只 override `write`,另一方向**不重写,继承基类 `InodeOps` 的默认实现(返回 -1)**。于是从读端 `write` 会得到 -1、从写端 `read` 会得到 -1——方向性是靠"哪个虚函数被 override"自然形成的,不用在 Ops 里查方向字段。这镜像了 Unix 管道模型:每个 fd 代表一个方向。

二是 **offset 参数留着却不读**。这是为了和 `InodeOps::read/write` 的虚函数签名完全一致(普通文件要 seek,所以带 offset),管道是字节流不可 seek,offset 对它无意义。签名对齐了,管道就能复用 `sys_read`/`sys_write` 的统一派发 `file->inode->ops->read/write(...)`,**不用为管道加任何专属 syscall 分支**。

### sys_read / sys_write 派发翻转:否则管道永远走不到

现在管道能伪装成文件了,但还有个顺序问题。看 [sys_read.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/sys_read.cpp) 的派发逻辑:

```cpp
int64_t sys_read(uint64_t fd, uint64_t buf_virt, uint64_t count, ...) {
    /* 规范地址校验 ... */
    cinux::fs::File* file = cinux::fs::g_global_fd_table().get(static_cast<int>(fd));
    if (file != nullptr && file->inode != nullptr && file->inode->ops != nullptr) {
        int64_t result = file->inode->ops->read(file->inode, file->offset, buf, count);
        if (result > 0) file->offset += result;
        return result;                      // ← 有 VFS 条目(如管道)就走这里
    }
    if (fd == 0) { /* 老的 PS/2 键盘轮询路径 */ }
    return -1;
}
```

031 把这个分支顺序**翻转**了:030 是"先判 `fd==0` 走键盘、再走 VFS",031 改成"先查 FDTable 走 VFS、再回退 `fd==0` 键盘"。`sys_write` 同理(先 VFS,再回退 `fd==1` 串口)。

为什么要翻?因为这一章 `init.cpp` 会把 fd 0 和 fd 1 **本身就绑成管道**(一个 `PipeReadOps`、一个 `PipeWriteOps`)。如果还按 030 的老顺序先判 `fd==0` 走键盘,那 `sys_read(0)` 就会一头扎进 PS/2 键盘路径,管道 stdin 永远读不到;`sys_write(1)` 会打到串口,管道 stdout 永远写不进。翻转之后,同一个 `fd==0` 既能是键盘(FDTable 无条目时),也能是管道读端(FDTable 有条目时),由"fd 是否在表里"决定走哪条路,而不是死看 fd 数字。这一翻,是整个回路能通的前提。

### sys_pipe 系统调用:SYS_pipe = 22

除了 `init.cpp` 的预接线,这一章还把"创建管道"做成了正式的系统调用 `sys_pipe`,编号 22——和 Linux x86_64 对齐(头文件注释明说 `numbers match Linux x86_64 where practical to simplify future porting`)。它干的活就是"new 一根 Pipe,包成两个 Inode,alloc 两个 fd,写回用户态":

```cpp
int64_t sys_pipe(uint64_t pipefd_virt, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    if (!is_user_addr(pipefd_virt)) return -1;          // 校验用户态指针
    auto* pipe = new cinux::ipc::Pipe();
    auto* read_ops  = new cinux::ipc::PipeReadOps(pipe);
    auto* write_ops = new cinux::ipc::PipeWriteOps(pipe);

    cinux::fs::Inode* read_inode  = new cinux::fs::Inode();
    read_inode->ops = read_ops;  read_inode->type = InodeType::Regular;
    cinux::fs::Inode* write_inode = new cinux::fs::Inode();
    write_inode->ops = write_ops; write_inode->type = InodeType::Regular;

    auto& table = cinux::fs::g_global_fd_table();
    int read_fd  = table.alloc(read_inode,  OpenFlags::RDONLY);
    int write_fd = table.alloc(write_inode, OpenFlags::WRONLY);
    /* 失败时按序回滚(close 已分配的 fd + delete 一堆对象) */

    auto* pipefd = reinterpret_cast<int32_t*>(pipefd_virt);
    pipefd[0] = read_fd;     // 约定:pipefd[0] 读端
    pipefd[1] = write_fd;    //         pipefd[1] 写端
    return 0;                // 返回 0(不是 fd),fd 经 int[2] 带出
}
```

注意几个和 `sys_read`/`sys_write` 不一样的地方:返回值是 0(成功)而不是字节数,两个 fd 经用户态的 `int[2]` 数组带出,`pipefd[0]` 读、`pipefd[1]` 写——这套 ABI 和 POSIX `pipe(int pipefd[2])` 完全一致。`Inode` 的 `type` 设成 `Regular` 而不是某种 "Pipe" 类型,因为派发链是 `inode->ops->read/write`,**完全不看 type**。`is_user_addr` 还会拒绝内核态地址(bit 47 置位)和 NULL,防止用户程序把内核指针传进来。这一章 shell 自己没用 `sys_pipe`(管道是 init 预接的),但 `test_sys_pipe` 把它当独立能力验证了。

### FDTable::set:把管道装进保留的 fd0/fd1

`sys_pipe` 用的是 `FDTable::alloc`,它会从 `FD_FIRST = 3` 开始扫,**跳过保留的 0/1/2**。可 `init.cpp` 要把管道装到 fd 0 和 fd 1,`alloc` 装不了。所以 031 给 [file.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/file.cpp) 的 `FDTable` 加了一个 `set`:

```cpp
bool FDTable::set(int fd, File* file) {
    auto guard = lock_.guard();
    if (fd < 0 || fd >= FD_TABLE_SIZE) return false;
    fds_[fd] = file;     // 强装到指定 fd(不释放原 File,所有权注释要求 caller 保证)
    return true;
}
```

"强装"语义,专门给保留的 0/1/2 用。它不 `delete` 原来occupying的 `File`(注释要求 caller 自己保证原 File 已正确释放),避免和"0/1/2 是外部保留槽"的约定冲突。

### init.cpp 接线:两条管道把 GUI 终端与 shell 焊起来

所有零件齐了,最后在 [init.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/init.cpp) 的 `kernel_init_thread` 里焊成回路。顺序很关键:先 `gui_start()` 拿到终端,再建管道,再绑 fd,再给终端接管道,最后才 `launch_first_user()` 起 shell:

```cpp
auto* term = cinux::gui::gui_start();              // 1. 拿到 Terminal*

// stdin 管道:Terminal on_key 写 → shell sys_read(0) 读
auto* stdin_pipe     = new cinux::ipc::Pipe();
auto* stdin_read_ops = new cinux::ipc::PipeReadOps(stdin_pipe);
auto* stdin_read_inode = new cinux::fs::Inode();
stdin_read_inode->ops = stdin_read_ops;
auto* stdin_file = new cinux::fs::File(stdin_read_inode, 0, cinux::fs::OpenFlags::RDONLY);
cinux::fs::g_global_fd_table().set(0, stdin_file);   // fd 0 = stdin 管道读端

// stdout 管道:shell sys_write(1) 写 → Terminal poll_output 读
auto* stdout_pipe      = new cinux::ipc::Pipe();
auto* stdout_write_ops = new cinux::ipc::PipeWriteOps(stdout_pipe);
auto* stdout_write_inode = new cinux::fs::Inode();
stdout_write_inode->ops = stdout_write_ops;
auto* stdout_file = new cinux::fs::File(stdout_write_inode, 0, cinux::fs::OpenFlags::WRONLY);
cinux::fs::g_global_fd_table().set(1, stdout_file);  // fd 1 = stdout 管道写端

term->set_stdin_pipe(stdin_pipe);                    // 2. 终端拿到同一根 Pipe*
term->set_stdout_pipe(stdout_pipe);

cinux::arch::launch_first_user();                    // 3. 起 ring-3 shell,它的 fd0/fd1 已就绪
```

这里方向千万别装反:fd 0 是 shell 要**读**的 stdin,所以挂 `PipeReadOps`;fd 1 是 shell 要**写**的 stdout,所以挂 `PipeWriteOps`。终端那边 `set_stdin_pipe`/`set_stdout_pipe` 接的是**同一根 `Pipe*` 对象**(不是 ops/inode),因为它要直接调 `Pipe::try_write`/`try_read`。`launch_first_user` 一执行,shell 就在已经接好 fd 0/1 的环境里跑起来了。

### 闭合回路

现在把 031 和 031b 的两半拼起来,看一个按键怎么变成屏幕上的字符。你敲 `h`:IRQ1 → 键盘 handler → `Mouse::event_queue()` → tick → `handle_key` → `Terminal::on_key`。因为这时 `stdin_pipe_` 非空,`on_key` 走管道分支,`stdin_pipe_->try_write('h')`。shell 在另一头 `sys_read(0)` 阻塞读,经 `FDTable::get(0)` → `PipeReadOps::read` → `Pipe::read` 拿到 `'h'`,放进行编辑缓冲并回显——回显时 `sys_write(1)` → `FDTable::get(1)` → `PipeWriteOps::write` → `Pipe::write` 写进 stdout 管道。下一拍 tick 里,`Terminal::poll_output` 用 `try_read` 把 `'h'` 抽出来,`write()` 落进字符缓冲,`render_to_canvas` 光栅化,`composite` 上屏。你看到了那个 `h`。

这一圈里,shell 始终在用标准的 `read`/`write` 操作字节流,它完全不知道对面是个 GUI 终端;终端也只在用 `try_write`/`try_read` 推/拉字节,它不认识 shell 的命令语义。两头都只对着"一根会回显的字节管道"编程,中间的解耦就是这整套设计的价值。

