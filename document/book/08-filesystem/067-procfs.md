---
title: 067 · ProcFS:把进程列表挂进 /proc
---

# 067 · ProcFS:把进程列表挂进 /proc

> 上一章同卷的 DevFS(064)立了一个「内存型虚拟 FS」的范式:`FileSystem` 子类 + `InodeOps` 子类 + boot 接线单独一个 `_init.cpp`。这一章把同一个范式用到别的地方——**进程自省**。Linux 有个 `/proc`,`ls /proc` 见到一堆数字(每个活进程一个 pid 目录),`cat /proc/<pid>/stat` 能读到那个进程的状态。这一章给 CinuxOS 立一个 ProcFS:`/proc` 根目录枚举活进程的 pid,`/proc/<pid>/` 下挂 `stat`、`cmdline` 这些「伪文件」——它们不在盘上,读它们时现从内核的进程表里取数、拼成文本返回。做完之后,`ls /proc` 能见到当前所有进程,`cat /proc/<pid>/stat` 能读到它的 pid、名字、状态、父进程。
>
> A 档:punchline 是 `/proc` 真挂上、`ls /proc` 见到 pid、`cat /proc/<pid>/stat` 读到进程信息。这一章真正要讲的是「**虚拟 FS 的根是动态的**」时怎么处理——DevFS 的根是一张固定节点表(null/zero/console 写死),ProcFS 的根是**当前活进程的 pid**,进程随时在创建和退出,这个目录是活的。还有「读伪文件 = 现场从内核结构生成文本」这套做法。一条诚实的边界先说在前头:这是进程自省的第一刀——只做 `/proc` 根枚举 pid + `/proc/<pid>/{stat,cmdline}`,不做 `/proc/version`/`meminfo`/`cpuinfo` 那些静态节点,也不做 `maps`/`fd`/`status`,留 follow-up。

## 这章咱们要点亮什么

1. **同一个范式换个场景**:DevFS 的「`FileSystem` 子类 + `InodeOps` 子类 + boot `_init.cpp`」范式直接套到进程自省。范式立一次,多处复用。
2. **虚拟 FS 的根是动态的时候怎么办**:DevFS 根是定长节点表,ProcFS 根是当前活 pid——靠定长 inode 池(PID 有上限)给每个 pid 一个稳定 inode,不靠单个 scratch 复用。
3. **readdir 枚举活进程**:给进程 registry 加一个「走到第 n 个」的 accessor(`signal_nth_task_pid`),readdir 按它列 pid。用「第 n 个」而不是「快照整个数组」,是为了不撑爆内核栈帧。
4. **伪文件 = 现场生成文本**:`/proc/<pid>/stat` 读的时候,从进程表取出那个 Task,现场拼成 `pid (name) state ppid ...` 文本返回。
5. **只露活进程**:每条路径 lookup 都校验 pid 还活着——进程在 readdir 和 lookup 之间退出了,就当作没这个 pid(对齐 Linux)。

## 同一个范式:ProcFs 是另一个虚拟 FS

DevFS 那章立的范式,这里照搬:`ProcFs : FileSystem`,设备/伪文件行为写成 `InodeOps` 的匿名 namespace 子类,boot 接线(`procfs::init()` 挂 `/proc`)单独放 `procfs_init.cpp`。所以这一章不重复讲范式本身(看 064),只讲 ProcFS 比 DevFS 多出来的两个新问题:**动态的根** 和 **伪文件的内容生成**。

## 问题一:根目录是动态的——定长 inode 池

DevFS 的根目录是一张**固定**节点表(`mount()` 时建好 null/zero/console,不变)。ProcFS 的根目录内容是**当前活进程的 pid**——进程随时在创建和退出,这个目录是活的,`ls /proc` 这一秒和下一秒见到的 pid 可能不一样。

这带来一个 inode 身份的问题。VFS 的规则是:`close` 一个 fd 只删 `File`、不删 `Inode`;所以每次 lookup 返回的 inode,必须由 ProcFS 拥有并且**长存**(不能是 lookup 时临时 new 的、用完就毁的)。DevFS 靠「定长节点表,节点 inode 长存」解决;ProcFS 的 pid 是动态的,不能照搬定长表。

> 解法靠一个事实:**pid 有上限**(`PID_MAX = 256`)。既然 pid 是 1..256 的有界整数,ProcFS 就预备一张**定长的 inode 池**,按 pid 索引——`pid_dir_inodes_[257]`(每个 pid 一个目录 inode)、`stat_inodes_[257]`、`cmdline_inodes_[257]`(`procfs.hpp:51`,`kProcPidMax = 256`,slot 0 留空)。每个 pid 永远映射到同一个稳定 inode(`ino = pid`,`fs_private = this`)。
>
> 这么做有个额外好处:**SMP 安全**。并发 lookup 不同的 pid,各取各的 inode,不竞态——要是用单个 scratch inode 复用(lookup pid 5 写进 scratch、lookup pid 6 又覆盖),两个核同时 lookup 就会读到错的 pid。一 pid 一稳定 inode,从根上避开这个竞态。`static_assert(kProcPidMax == PidAllocator::PID_MAX)` 把池的上界跟 pid allocator 锁死,allocator 改了立刻编译期报。

`lookup` 就解析路径(strip 掉 `/proc` 前缀后,rel_path 形如 `""` / `"/1"` / `"/1/stat"`):strip 前导 `/`、`parse_pid` 解析前导十进制(溢出 `PID_MAX` 即拒,永不越界索引池)、`<pid>` 返回 pid 目录 inode、`<pid>/stat` 或 `<pid>/cmdline` 返回伪文件 inode。

## 问题二:readdir 枚举活进程——「第 n 个」而非快照

`/proc` 根目录的 `readdir` 要列出当前所有活 pid。朴素做法是「快照整个 registry 到一个数组,再遍历」。可这在内核栈上要 `int pids[kProcPidMax+1]` —— 1028 字节,**超过内核的 `-Wframe-larger-than=1024` 门槛**(production `big_kernel_common` 是 `-Werror`,不像 test/ 只警告)。

解法是给进程 registry 加一个**「走到第 n 个」**的 accessor,而不是「给我全部」:

```cpp
bool signal_nth_task_pid(uint32_t n, int* out_pid);   // signal.hpp:207
```

(`signal.cpp:89`。)它在 registry 锁内走到第 n 个 task,把那个 pid 写出来;走不到(n 大于当前 task 数)返 false。`ProcRootDirOps::readdir` 就按索引要:索引 0/1 是 `.` / `..`,索引 i≥2 问「第 (i-2) 个活 pid」。全程无栈数组,栈帧小。代价是全 `/proc` 列举是 O(tasks²)(每列一项从头走到第 n 个),但 tasks ≤ 256,可忽略。

> 这个 accessor 是**纯增量**——只加 `signal_nth_task_pid`,不改 registry 的 register/unregister/find_by_pid。给一个已有数据结构加「按序访问」的只读口子,不动它的写路径,是最安全的加法。`-Wframe-larger-than` 这种门槛看着烦,但它逼你想「不要在内核栈上放大数组」——这本身是条好纪律(内核栈才 16 KB,前面 path 那个 4 KB 缓冲、UDP 那个 1.5 KB 包,都是同一类约束)。

## 伪文件:读的时候现场生成文本

`/proc/<pid>/stat` 和 `cmdline` 是伪文件——没有磁盘内容,read 时现场从进程表取数、拼文本。内容生成器单拆一个文件 `procfs_content.cpp`(`format_proc_stat` / `format_proc_cmdline`,跟 FS 的 plumbing 分开,各自不超过 500 行软限):

```cpp
uint32_t format_proc_stat(const Task* t, char* buf, uint32_t cap) {
    // pid (name) state ppid tgid uid gid\n   —— Linux /proc/<pid>/stat 的简化子集
    b.put_u(t->pid);  b.put(" (");  b.put(t->name);  b.put(") ");
    b.put(task_state_char(t->state));   // R/S/T/Z/X 单字符
    b.put_u(t->ppid);  ... ;
}
```

(`procfs_content.cpp:77`。)`cmdline` 是 task 名字 + NUL(CinuxOS 的 Task 不存 argv,这是 best-effort 暴露 comm;真 argv 跟踪留 follow-up)。read 时 `signal_find_task_by_pid` 取出 Task、生成内容到栈 buffer、按 offset 拷贝(支持 pread 和 EOF)。

## 只露活进程 + 一个 TOCTOU 边界

每条路径的 lookup 都经 `signal_find_task_by_pid` 校验 pid 还活着——进程在 readdir 之后、lookup 之前退出了,lookup 就当没这个 pid(返 NotFound)。这跟 Linux 一致:`/proc` 只露当前活进程。

> 这里有个诚实的边界要交代:registry 的访问有个 TOCTOU 窗口。`signal_find_task_by_pid` 在锁内返回 Task 指针后就解锁了,read 它的字段时(拼 stat 文本),这个 task 可能已经被另一个核 exit + free 了——指针悬垂。窗口极小(find 后立刻快照字段),作为 hobby OS 可接受;`test_stat_read_dead_pid_is_not_found` 实证了「unregister 后读同一 inode → NotFound」。真正的修要等 registry 变成 RCU-safe(后续工程债:registry RCU 化),那是另一条线。这里先记着,不假装没问题。

## §14 文件门:这一章不能 host 单测

064 DevFS 靠 `CharSink` 注入缝,让核心逻辑(host 能链的那份)可单测。ProcFS 不一样——它**直读 kernel registry**(`signal.hpp` / `process.hpp`),没有注入缝,host 链不了。所以这一章的测试走 kernel harness(QEMU 里 `run_procfs_tests`,11 测),不靠 host 单测。boot 接线(`kprintf`)照例独立 `procfs_init.cpp`,CMake 决定编不编,源码零 `#ifdef`(同 DevFS 的 §14 文件门)。

## 验证

三层验证(没有 host 单测这层)。

**第一层:kernel 测试,ProcFS 机制。** `kernel/test/test_procfs.cpp` 十一例:mount、根 readdir 枚举 pid、lookup `<pid>` 目录、lookup `<pid>/stat` / `<pid>/cmdline` 伪文件、stat 内容格式对、非活 pid → NotFound、release 后读 → NotFound。测试自建栈 Task 注册已知 pid(测试内核主线程不进 registry,不能依赖残留状态),确定性验证。

**第二层:boot 冒烟。** `make run` 起 QEMU,看 `[PROCFS] mounted at /proc`(`procfs::init()` 在 `devfs::init()` 之后挂)。`ls /proc` 见到 pid,`cat /proc/<pid>/stat` 读到 `pid (name) state ppid ...`。

**第三层:全量回归。** 改了公共头 `signal.hpp`(加 `signal_nth_task_pid`),push 前全量编(含 host)。`run-kernel-test-all` 两腿各 **997 passed / 0 failed**(986 基线 + 11 procfs)。

## 这章没做的

- **静态节点**:`/proc/version`、`/proc/meminfo`、`/proc/cpuinfo`、`/proc/uptime` 这些不依赖进程的节点没做,留 follow-up。
- **更多每进程伪文件**:`/proc/<pid>/maps`(地址空间)、`/proc/<pid>/fd`(打开文件)、`/proc/<pid>/status` 没做。
- **完整 Linux `/proc/<pid>/stat`**:Linux 的 stat 有 52 个字段(含 per-task accounting),CinuxOS 没有那些 accounting,只做简化子集。
- **真 argv**:Task 不存 argv,`cmdline` 只暴露 comm(名字)。真 NUL 分隔的 argv 要 Task 存 argv,留 follow-up。
- **registry TOCTOU**:find 后 unlock、read 时 task 可能已 free 的窗口,真修要 registry RCU-safe(后续工程债:registry RCU 化)。
- **host 可测**:ProcFS 直读 registry 无注入缝,核心逻辑 host 链不了。要 host 可测得抽个 `TaskInfoProvider` 接口(同 DevFS CharSink 缝),留 follow-up。

## 小结

- ProcFS 照 DevFS 范式(`FileSystem` 子类 + `InodeOps` 子类 + boot `_init.cpp`),用到进程自省:`/proc` 根枚举活 pid,`/proc/<pid>/{stat,cmdline}` 现场生成文本。
- 新问题是**动态的根**:DevFS 根是定长节点表,ProcFS 根是当前活 pid。靠 PID 有上限(256),预备定长 inode 池按 pid 索引,每 pid 一个稳定 inode——顺带 SMP 安全(无 scratch 复用竞态)。
- readdir 用 `signal_nth_task_pid`(「第 n 个」accessor)而非快照数组,避开栈上大数组超 `-Wframe-larger-than`;纯增量,不改 registry 写路径。
- 伪文件 read 时 `signal_find_task_by_pid` 取 Task、`format_proc_stat`/`format_proc_cmdline` 现场拼文本;每条 lookup 校验 pid 活着,只露活进程。
- 诚实边界:TOCTOU 窗口(find 后 unlock)留 registry RCU-safe(后续工程债:registry RCU 化);ProcFS 直读 registry 无注入缝,不能 host 单测,走 kernel harness;静态节点/maps/fd/真 argv 留 follow-up。
