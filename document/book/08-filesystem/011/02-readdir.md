---
title: 02 · 动态根目录:定长 inode 池与 readdir 枚举活进程
---

# 动态根目录:定长 inode 池与 readdir 枚举活进程

## 问题一:根目录是动态的——定长 inode 池

DevFS 的根目录是一张**固定**节点表(`mount()` 时建好 null/zero/console,不变)。ProcFS 的根目录内容是**当前活进程的 pid**——进程随时在创建和退出,这个目录是活的,`ls /proc` 这一秒和下一秒见到的 pid 可能不一样。

这带来一个 inode 身份的问题。VFS 的规则是:`close` 一个 fd 只删 `File`、不删 `Inode`;所以每次 lookup 返回的 inode,必须由 ProcFS 拥有并且**长存**(不能是 lookup 时临时 new 的、用完就毁的)。DevFS 靠「定长节点表,节点 inode 长存」解决;ProcFS 的 pid 是动态的,不能照搬定长表。

> 解法靠一个事实:**pid 有上限**(`PID_MAX = 256`)。既然 pid 是 1..256 的有界整数,ProcFS 就预备一张**定长的 inode 池**,按 pid 索引——`pid_dir_inodes_[257]`(每个 pid 一个目录 inode,`procfs.hpp:124`)、`stat_inodes_[257]`(`procfs.hpp:128`)、`cmdline_inodes_[257]`(`procfs.hpp:131`)(`kProcPidMax = 256` 定义在 `procfs.hpp:59`,slot 0 留空)。每个 pid 永远映射到同一个稳定 inode(`ino = pid`,`fs_private = this`)。
>
> 这么做有个额外好处:**SMP 安全**。并发 lookup 不同的 pid,各取各的 inode,不竞态——要是用单个 scratch inode 复用(lookup pid 5 写进 scratch、lookup pid 6 又覆盖),两个核同时 lookup 就会读到错的 pid。一 pid 一稳定 inode,从根上避开这个竞态。`static_assert(kProcPidMax == PidAllocator::PID_MAX)` 把池的上界跟 pid allocator 锁死,allocator 改了立刻编译期报。

`lookup` 就解析路径(strip 掉 `/proc` 前缀后,rel_path 形如 `""` / `"/1"` / `"/1/stat"`):strip 前导 `/`、`parse_pid` 解析前导十进制(溢出 `PID_MAX` 即拒,永不越界索引池)、`<pid>` 返回 pid 目录 inode、`<pid>/stat` 或 `<pid>/cmdline` 返回伪文件 inode。

## 问题二:readdir 枚举活进程——「第 n 个」而非快照

`/proc` 根目录的 `readdir` 要列出当前所有活 pid。朴素做法是「快照整个 registry 到一个数组,再遍历」。可这在内核栈上要 `int pids[kProcPidMax+1]` —— 1005 字节,**超过内核的 `-Wframe-larger-than=1024` 门槛**(production `big_kernel_common` 是 `-Werror`,不像 test/ 只警告)。

解法是给进程 registry 加一个**「走到第 n 个」**的 accessor,而不是「给我全部」:

```cpp
bool signal_nth_task_pid(uint32_t n, int* out_pid);   // signal.hpp:219
```

(`signal.cpp:115`。)它在 registry 锁内走到第 n 个 task,把那个 pid 写出来;走不到(n 大于当前 task 数)返 false。`ProcRootDirOps::readdir`(`procfs.cpp:168-209`)就按索引要:索引 0/1 是 `.` / `..`,索引 2 是 `/proc/meminfo` 固定伪文件,索引 3 是 `/proc/cpuinfo`,索引 i≥4 才问「第 (i-4) 个活 pid」(`procfs.cpp:204`)。全程无栈数组,栈帧小。代价是全 `/proc` 列举是 O(tasks²)(每列一项从头走到第 n 个),但 tasks ≤ 256,可忽略。

> 这个 accessor 是**纯增量**——只加 `signal_nth_task_pid`,不改 registry 的 register/unregister/find_by_pid。给一个已有数据结构加「按序访问」的只读口子,不动它的写路径,是最安全的加法。`-Wframe-larger-than` 这种门槛看着烦,但它逼你想「不要在内核栈上放大数组」——这本身是条好纪律(内核栈才 16 KB,前面 path 那个 4 KB 缓冲、UDP 那个 1.5 KB 包,都是同一类约束)。
