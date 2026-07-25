---
title: 073 · busybox 跑起来 + socket API:用户态生态的试金石
---

# 073 · busybox 跑起来 + socket API:用户态生态的试金石

> 059 让 musl 静态 hello 跑起来了。可 hello 只是打印一行——真正的用户态生态,得跑得起**真实的程序**。这一章的 punchline 就是:**静态 busybox 1.36(musl 编译)在 CinuxOS 上真跑通,14 个 applet 全过**——`echo`、`id`(出 `uid=0(root)`)、`whoami`(`root`)、`pwd`(`/`)、`cat`、`ls`、`ps`、`free`(`Mem: 9437184 ...`)、`uname`、`wc`、`sleep`、`env`、`hostname`、`true`/`false`。busybox 是 Linux 用户态的试金石——它能跑,说明内核的 syscall ABI、文件系统、进程、内存统计都到了「真程序能用」的程度。为了 busybox 跑得起来,这一章一口气补了一大堆它用到的 syscall(`getdents64` 给 `ls`、`chmod`/`chown`/`link`/`rename`/`utimensat` 给文件操作、`dup`/`fcntl`/`nanosleep`/`sysinfo`/`getrusage` 给各种),外加 `/proc/meminfo`(给 `free`)。顺带还把 **socket API** 做了(`socket`/`bind`/`connect`/`listen`/`accept`/`sendto`/`recvfrom`,让 063/069 那两个协议层 UDP/TCP 真正对用户态可用——loopback 上 UDP + TCP echo 干通)。
>
> A 档:punchline 是 busybox 14 applet 真跑出真输出(不是机制测绿,是真二进制 fork+execve 看串口输出 + 退出码)+ socket loopback echo。这是个**大 bundle**(用户态生态 + socket),这一章聚焦两条主线(busybox 试金石、socket ABI),中间那一大坨支撑 syscall一笔带过。诚实的边界:TCP 还是最小可用(无重传/RTO,留 HPET 周期中断),musl socket demo 留 follow-up。

## 这章咱们要点亮什么

1. **busybox 是用户态试金石**:它能跑,说明 syscall ABI/FS/进程/内存统计都到「真程序能用」。从 musl hello(059)到 busybox 是质变——hello 打印一行,busybox 是真生态。
2. **机制测绿 ≠ 真二进制能跑**:每一批 syscall 都有机制测(绿),但 busybox 真跑时才发现缺 `/hello`、`/proc/meminfo`、`/etc/passwd` 这些**环境件**(非内核 bug,是盘/测试环境缺)。真二进制验收不可替代。
3. **Socket = InodeOps 子类**(对齐 PTY/pipe):socket fd 就是一个 pipe fd,`sys_read/write/ioctl/close` 零改动;per-socket RX 环 + 阻塞 + accept 队列放 Socket 适配器,协议层(UDP/TCP)保持纯。
4. **协议层跟用户态的缝是 listener 回调**:Socket 适配器挂 `UdpListener::on_udp` / `TcpListener::on_accept/on_data`,回调里拷贝帧进 per-socket 环(设备 dispatch 后回收 buffer,故必拷)。

## 主线一:busybox 试金石——从 hello 到真生态

059 的 musl hello 是个里程碑(对齐 Linux ABI + 初始栈),但它只打印一行。要证明内核真的「能用」,得跑真程序。busybox 是个自包含的多功能二进制(一个 `/bin/busybox` 加 applet 名当 `ls`/`cat`/`ps`/... 用),它用到的 syscall 面比 hello 广得多——这就是试金石。

要 busybox 跑起来,补了它用到的一整批 syscall(每个都有机制测):

- **`getdents64`** —— `ls` 列目录靠它(读目录项);
- **`chmod` / `chown` / `utimensat`** —— 改文件属性/属主/时间戳;
- **`link` / `rename` / `symlink`** —— 硬链接、改名、软链接(给 Ext2 加了这些 InodeOps override);
- **`dup` / `fcntl`** —— 复制 fd、fd 控制(重定向、close-on-exec 那些的基础);
- **`nanosleep` / `sysinfo` / `getrusage`** —— `sleep`、系统信息(`free` 用 sysinfo)、资源用量;
- **`getdents64` 之外的 vfs**:补全 ls/ps/free 要的路径。

外加几个**环境件**(全是盘/测试环境缺,不是内核 bug):`/proc/meminfo`(ProcFS 新伪文件,从 PMM 生成 MemTotal/Free,给 `free` grep)、`/etc/passwd` + `/etc/group`(给 `whoami`/`id` 解析 uid 0→root)、测试内核挂 `/proc`(给 `ps`/`free`)。

> **这一段最大的教训**:这一章每批 syscall 都写了机制测(绿),可 busybox 真跑起来是另一回事——一开始 9/14(5 个因为缺 `/hello`、`/proc`、`/etc/passwd`,busybox 自己错退,不是内核错)。补齐环境件后 14/14。**机制测绿只证明「这个 syscall 的逻辑对」,不证明「真程序能跑」——真二进制验收(fork+execve busybox 看串口真输出 + 退出码)是不可替代的一环**。以后凡是有「跑真程序」目标的里程碑,都得过这一关,不能只靠机制测。

## 主线二:socket API——让协议层对用户态可用

063(UDP)和 069(TCP)立了协议层,可那是**内核测试接口**——没 `socket()` ABI,用户态程序用不了。这一章把那两个协议层接到用户态,立完整的 socket API。

**架构决定(关键)**:`Socket = InodeOps 子类`,对齐 PTY/pipe——一个 socket fd 就是一个 pipe fd(`fd → File → Inode → SocketOps`)。于是 `sys_read` / `sys_write` / `sys_ioctl` / `sys_close` **零改动**就对 socket 生效(socket fd 跟 pipe fd 走同一套 fd 派发)。这比「给 File 加个类型 tag」干净得多(那要改每个 fd 消费者 + 未来的 dup2/poll,正是 Linux `struct file` 单缝避免的反模式)。

- `socket()` 装合成一个 Inode(抄 `sys_pipe` 的 unique_ptr → FDTable::alloc → release);
- `accept()` 抄 `PtmxOps::open` 的 cloning,再发一个 fd(accepted child);
- `close()` 走现成 FDTable::close;SocketOps 析构拆连接(TCP 发 FIN)。
- SocketOps 是单一共享无状态实例;per-fd 的 `Socket*` 存 `inode->fs_private`(PTY 范式)。

**协议层保持纯**:`TcpModule` / `UdpModule`(069/063 立的)一行不动。per-socket 的 RX 环 + 阻塞 + accept 队列放进 **Socket 适配器**(文件 `udp_socket.hpp` / `tcp_socket.hpp` 里的 `UdpSocket` / `TcpSocket` 类),挂在协议层的 listener 缝上:`UdpListener::on_udp` / `TcpListener::on_accept/on_data/on_close`。

> 回调里有个必须的细节:**拷贝借来的帧**。`on_udp` / `on_data` 收到的 `FrameView` 是借自设备 buffer 的,设备 dispatch 一返回就回收;所以回调里要**把帧 copy 进 per-socket 环**(udp.hpp/tcp.hpp 注释明示),不能存指针。UDP 用定长 Datagram 环(每包 {src, port, len, data}),TCP 是字节流用 `RingBuffer<uint8_t>`(像 pipe,on_data push_batch,recv pop_batch)。
>
> 阻塞(recv 等数据、accept 等连接)用 `prepare_to_wait` / `schedule_blocked` / `unblock`(F8 pipe 那套),从 `net_poll` kthread 上下文的 listener 回调唤醒。**绝不 sti/hlt**(在 syscall 上下文 sti → #DF,跟 059 sys_ping / 071 pipe 阻塞同根——详见 071《pipe & FIFO》对 syscall 里 sti 把时钟中断放进陷阱帧窗口 → sysretq 弹花 → #DF 的剖析)。

socket syscall 用 Linux x86_64 标准号(`socket`=41 / `connect`=42 / `accept`=43 / `sendto`=44 / `recvfrom`=45 / `bind`=49 / `listen`=50,syscall_nums.hpp 41-50 范围内除 socket 这批外仍空),`sockaddr_in` 的 port 是网络序(musl 大端铺,syscall handler `byte_swap16` 转)。loopback 上 UDP echo(client→server→client)+ TCP echo(connect→握手→accept→双向 echo)都干通。

> **tag-bound 提醒**:073 当时 40-55 全空无撞号;后续 socketpair/setsockopt/getsockopt 又填了 53/54/55(`SYS_accept4`=288 也补了)。读者按 073 tag 读源码即可。

## 顺带:kernel/fs 分子目录

29 个文件平铺在 `kernel/fs/` 太乱。这一章按性质分了子目录:核心 VFS(inode/file/vfs_mount/vfs_filesystem/stat/path)留 `kernel/fs/` 根,4 个后端各进子目录——`ext2/`(8 文件)、`procfs/`(5)、`devfs/`(3)、`ramdisk/`(3)。纯结构整理(git mv + include 路径批量改),零功能变。

> **tag-bound 提醒**:073 当时 ext2 还在 `kernel/fs/ext2/`(8 文件);后来 080 章 wholesale 把 ext2 整个搬出 `kernel/fs/`,独立成库 `libs/ext2/`(13 个文件,加了 ext2_links/ext2_metadata/ext2_extent 等)。本节描述的是 073 tag 当时的整理动作,读者按 tag 读源码即可,别去 `kernel/fs/ext2/` 扑空。procfs/devfs/ramdisk 三个仍在 `kernel/fs/` 下没挪。

## 验证

**真二进制验收(busybox)**:build-musl.sh 编 musl sysroot → 拉 busybox 1.36 源 → musl-gcc static 编 → 进 ext2 盘 → QEMU `fork+execve /bin/busybox <applet>` 看串口真输出 + 退出码。14 applet 双腿全过(echo / id(`uid=0(root)`) / whoami(`root`) / pwd(`/`) / cat / wc / uname / true / false(exit 1) / sleep 0 / env / hostname / ls / ps / free(`Mem: 9437184 ...`))。

**socket loopback**:UDP echo(client→server→client round-trip)+ TCP echo(connect→握手→accept→双向 echo),两腿过。

**机制测**:`test_socket`(socket ABI:出 fd / 参数校验 / fd→Socket 路由 / close + UDP/TCP echo)、`test_sys_getdents`(ls 的目录读)、各 syscall 机制测。

`run-kernel-test-all` 两腿各 **1061 passed / 0 failed**(1020 基线 + 41 用户态生态/socket 测),`check_net_decoupling` 绿(kernel/net/ 不 include driver)。

## 这章没做的

- **TCP 可靠性**:socket 接通了,但 TCP 仍是最小可用(无重传/RTO/窗口/拥塞,069 的范围)。loopback 零丢包所以 echo 稳;SLIRP 真网可能丢。要内核 timer(HPET 周期中断,F5-M4 follow-up)。
- **musl socket demo**:这一章的 socket 验证靠内核测试接口 + loopback echo;真 musl 程序用 socket ABI 的 demo 显式留 follow-up(要建 musl sysroot + 给 test kernel 加 gated net_poll 启动)。
- **socket close 资源释放**:073 当时无 `InodeOps::release` 钩子(close 不彻底拆 Socket);后续 InodeOps 加了 `release` 虚槽(`kernel/fs/inode.hpp:228`,`virtual void release(Inode* inode)`,注释明示「a socket unbinds / sends FIN」),socket close 的资源释放已落地。诚实边界:pipe/FIFO 自己的 close-propagation 还没做(需 end-refcounting,是单独的 DEBT,inode.hpp 注释里写着「FIFOs (whose close-propagation needs end-refcounting -- a separate DEBT)」)。
- **一堆 socket 精化**:`setsockopt`(stub)、`getsockname`/`getpeername`/`accept4` 精化、IPv6、AF_UNIX(F8)、sendmsg cmsg、epoll(F8)——都留 follow-up。

## 小结

- **busybox 是用户态试金石**:静态 busybox 1.36(musl)在 CinuxOS 真跑通 14 applet,说明 syscall ABI/FS/进程/内存统计都到「真程序能用」。从 hello(059)到 busybox 是质变。
- 补了 busybox 要的一整批 syscall(getdents64/chmod/chown/link/rename/utimensat/dup/fcntl/nanosleep/sysinfo/getrusage)+ /proc/meminfo 环境件。
- 教训:**机制测绿 ≠ 真二进制能跑**。每批 syscall 机制测都绿,可 busybox 真跑才发现缺环境件(/proc/meminfo、/etc/passwd)。真二进制验收(fork+execve 看输出)不可替代。
- **Socket = InodeOps 子类**(对齐 PTY/pipe):socket fd 就是 pipe fd,sys_read/write/close 零改动;协议层(UDP/TCP)保持纯,per-socket RX 环 + 阻塞 + accept 队列放 Socket 适配器挂 listener 缝,回调拷贝帧进环。阻塞用 wait queue(不 sti/hlt)。loopback UDP+TCP echo 干通。
- kernel/fs 分子目录(ext2//procfs//devfs//ramdisk/)。诚实边界:TCP 仍最小可用(重传留 HPET)、musl socket demo 留 follow-up(socket close 资源释放后续已由 InodeOps::release 虚槽落地)。
