---
title: 067 · ProcFS:把进程列表挂进 /proc
---

# 067 · ProcFS:把进程列表挂进 /proc

> 上一章同卷的 DevFS(064)立了一个「内存型虚拟 FS」的范式:`FileSystem` 子类 + `InodeOps` 子类 + boot 接线单独一个 `_init.cpp`。这一章把同一个范式用到别的地方——**进程自省**。Linux 有个 `/proc`,`ls /proc` 见到一堆数字(每个活进程一个 pid 目录),`cat /proc/<pid>/stat` 能读到那个进程的状态。这一章给 CinuxOS 立一个 ProcFS:`/proc` 根目录枚举活进程的 pid,`/proc/<pid>/` 下挂 `stat`、`cmdline` 这些「伪文件」——它们不在盘上,读它们时现从内核的进程表里取数、拼成文本返回。做完之后,`ls /proc` 能见到当前所有进程,`cat /proc/<pid>/stat` 能读到它的 pid、名字、状态、父进程。
>
> A 档:punchline 是 `/proc` 真挂上、`ls /proc` 见到 pid、`cat /proc/<pid>/stat` 读到进程信息。这一章真正要讲的是「**虚拟 FS 的根是动态的**」时怎么处理——DevFS 的根是一张固定节点表(null/zero/console 写死),ProcFS 的根是**当前活进程的 pid**,进程随时在创建和退出,这个目录是活的。还有「读伪文件 = 现场从内核结构生成文本」这套做法。一条诚实的边界先说在前头:这是进程自省的第一刀——做 `/proc` 根枚举 pid + `/proc/<pid>/{stat,cmdline}`,外加 `/proc/meminfo`/`/proc/cpuinfo` 两个静态节点(都从 `g_pmm`/`g_acpi_info` 现场生成);`/proc/version`/`uptime` 静态节点和 `maps`/`fd`/`status` 每进程伪文件还不做,留 follow-up。

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

> 解法靠一个事实:**pid 有上限**(`PID_MAX = 256`)。既然 pid 是 1..256 的有界整数,ProcFS 就预备一张**定长的 inode 池**,按 pid 索引——`pid_dir_inodes_[257]`(每个 pid 一个目录 inode,`procfs.hpp:124`)、`stat_inodes_[257]`(`procfs.hpp:128`)、`cmdline_inodes_[257]`(`procfs.hpp:131`)(`kProcPidMax = 256` 定义在 `procfs.hpp:59`,slot 0 留空)。每个 pid 永远映射到同一个稳定 inode(`ino = pid`,`fs_private = this`)。
>
> 这么做有个额外好处:**SMP 安全**。并发 lookup 不同的 pid,各取各的 inode,不竞态——要是用单个 scratch inode 复用(lookup pid 5 写进 scratch、lookup pid 6 又覆盖),两个核同时 lookup 就会读到错的 pid。一 pid 一稳定 inode,从根上避开这个竞态。`static_assert(kProcPidMax == PidAllocator::PID_MAX)` 把池的上界跟 pid allocator 锁死,allocator 改了立刻编译期报。

`lookup` 就解析路径(strip 掉 `/proc` 前缀后,rel_path 形如 `""` / `"/1"` / `"/1/stat"`):strip 前导 `/`、`parse_pid` 解析前导十进制(溢出 `PID_MAX` 即拒,永不越界索引池)、`<pid>` 返回 pid 目录 inode、`<pid>/stat` 或 `<pid>/cmdline` 返回伪文件 inode。

## 问题二:readdir 枚举活进程——「第 n 个」而非快照

`/proc` 根目录的 `readdir` 要列出当前所有活 pid。朴素做法是「快照整个 registry 到一个数组,再遍历」。可这在内核栈上要 `int pids[kProcPidMax+1]` —— 1028 字节,**超过内核的 `-Wframe-larger-than=1024` 门槛**(production `big_kernel_common` 是 `-Werror`,不像 test/ 只警告)。

解法是给进程 registry 加一个**「走到第 n 个」**的 accessor,而不是「给我全部」:

```cpp
bool signal_nth_task_pid(uint32_t n, int* out_pid);   // signal.hpp:219
```

(`signal.cpp:115`。)它在 registry 锁内走到第 n 个 task,把那个 pid 写出来;走不到(n 大于当前 task 数)返 false。`ProcRootDirOps::readdir`(`procfs.cpp:168-209`)就按索引要:索引 0/1 是 `.` / `..`,索引 2 是 `/proc/meminfo` 固定伪文件,索引 3 是 `/proc/cpuinfo`,索引 i≥4 才问「第 (i-4) 个活 pid」(`procfs.cpp:204`)。全程无栈数组,栈帧小。代价是全 `/proc` 列举是 O(tasks²)(每列一项从头走到第 n 个),但 tasks ≤ 256,可忽略。

> 这个 accessor 是**纯增量**——只加 `signal_nth_task_pid`,不改 registry 的 register/unregister/find_by_pid。给一个已有数据结构加「按序访问」的只读口子,不动它的写路径,是最安全的加法。`-Wframe-larger-than` 这种门槛看着烦,但它逼你想「不要在内核栈上放大数组」——这本身是条好纪律(内核栈才 16 KB,前面 path 那个 4 KB 缓冲、UDP 那个 1.5 KB 包,都是同一类约束)。

## 伪文件:读的时候现场生成文本

`/proc/<pid>/stat` 和 `cmdline` 是伪文件——没有磁盘内容,read 时现场从进程表取数、拼文本。内容生成器单拆一个文件 `procfs_content.cpp`(`format_proc_stat` / `format_proc_cmdline`,跟 FS 的 plumbing 分开,各自不超过 500 行软限):

```cpp
uint32_t format_proc_stat(const cinux::proc::TaskSnapshot& s, char* buf, uint32_t cap) {
    // pid (name) state ppid tgid uid gid\n   —— Linux /proc/<pid>/stat 的简化子集
    return cinux::fmt::format(buf, cap, "{} ({}) {} {} {} {} {}\n",
                              static_cast<uint32_t>(s.pid), s.name,
                              task_state_char(s.state),   // R/S/T/Z/X 单字符
                              static_cast<uint32_t>(s.ppid),
                              static_cast<uint32_t>(s.tgid), s.uid, s.gid);
}
```

(`procfs_content.cpp:45` 是 stat,`:55` 是 cmdline;声明在 `procfs_content.hpp:59/65`。)`cmdline` 是 task 名字 + NUL(CinuxOS 的 Task 不存 argv,这是 best-effort 暴露 comm;真 argv 跟踪留 follow-up)。read 时 `signal_snapshot_task` 把字段快照到栈上的 `TaskSnapshot`、再 `format_proc_stat` 拼内容到栈 buffer、按 offset 拷贝(支持 pread 和 EOF)。**注意是 `TaskSnapshot&` 不是 `Task*`**——之所以走快照而不直接取裸指针,是为了避开 TOCTOU:registry 锁内拷字段、锁外只 format(见下一节的 DEBT-022)。

## 静态伪文件节点:procfs_pseudo

`/proc/<pid>/stat` 是「输入随进程走」的伪文件——每次读,从某个 Task 的字段生成。`/proc` 根目录下还有一类**跟进程无关**的伪文件:`/proc/meminfo`、`/proc/cpuinfo`。它们的内容来自内核的**全局状态**(`g_pmm` 的页计数、`g_acpi_info` 的 MADT),不挂在任何一个 pid 下。这套「静态伪文件」的 InodeOps 单独放 `procfs_pseudo.cpp`,跟 `procfs.cpp` 的 plumbing 分开——理由还是那条 500 行软限:meminfo/cpuinfo 两个 `InodeOps` 子类塞进 `procfs.cpp` 会让它过 cap,拆出去干净。

跟前面 pid 伪文件对照一下,机制是**同一个范式**、只是数据源换了:

| 节点 | 数据源 | inode 身份 | readdir 露面 |
|------|--------|-----------|--------------|
| `/proc/<pid>/stat` | Task 字段(`signal_snapshot_task`) | 池里按 pid 索引(`stat_inodes_[pid]`) | 否(在 pid 子目录下) |
| `/proc/meminfo` | 全局 `g_pmm` | 单个固定 `meminfo_inode_`(ino=0xF6) | 是(根 readdir index 2) |
| `/proc/cpuinfo` | 全局 `g_acpi_info` | 单个固定 `cpuinfo_inode_`(ino=0xF7) | 是(根 readdir index 3) |

pid 伪文件靠「定长 inode 池按 pid 索引」解决身份;静态伪文件**只有一个 inode**,`mount()` 时就建好、`ino` 取个跟任何 pid 都不撞的固定值(0xF6 / 0xF7,pid 永远在 1..256)。它不需要池,因为它没有「同一类节点的多个实例」——meminfo 就一份。

### InodeOps:read 时现从内核全局变量生成

`ProcMeminfoFileOps::read`(`procfs_pseudo.cpp:53`)就是这套范式的全部:

```cpp
ErrorOr<int64_t> read(const Inode* inode, uint64_t offset, void* buf, uint64_t count) override {
    if (inode == nullptr || buf == nullptr) {
        return Error::InvalidArgument;
    }
    constexpr uint32_t kBytesPerKb = 1024;
    constexpr uint32_t kPageBytes  = 4096;
    uint32_t           total_kb =
        static_cast<uint32_t>(cinux::mm::g_pmm.total_page_count() * kPageBytes / kBytesPerKb);
    uint32_t free_kb =
        static_cast<uint32_t>(cinux::mm::g_pmm.free_page_count() * kPageBytes / kBytesPerKb);
    char     line[kProcMeminfoMax];
    uint32_t len = format_proc_meminfo(total_kb, free_kb, line, sizeof(line));
    return copy_pseudo(line, len, offset, buf, count);
}
```

注意它**不在 mount 时生成内容、缓存起来**——每次 read 都重新算 total/free 页数、重新 `format_proc_meminfo` 拼一遍。这正是伪文件的本意:文件没有持久内容,「内容」是「此刻内核状态的快照」。下次读 free 会变,因为内存被分配/释放了;缓存就把这点打破了。`ProcCpuinfoFileOps::read`(`procfs_pseudo.cpp:80`)同构,数据源换成 `g_acpi_info` 的 `cpu_count` + `cpu_apic_ids`(MADT 里每 CPU 的 apic id)。

`copy_pseudo`(`procfs_pseudo.cpp:38`)是这段范式的另一半——给定生成好的内容(len 字节),按 offset/count 拷到用户 buffer,offset 越界返 0(EOF)。它跟 stat/cmdline 的 read 复用的是同一个拷贝语义,只是内容来源不同:

```cpp
ErrorOr<int64_t> copy_pseudo(const char* content, uint32_t len, uint64_t offset, void* buf,
                             uint64_t count) {
    if (offset >= len) {
        return 0;
    }
    uint64_t avail = static_cast<uint64_t>(len) - offset;
    uint64_t n     = count < avail ? count : avail;
    memcpy(buf, content + offset, n);
    return static_cast<int64_t>(n);
}
```

`fill_file_stat`(`procfs_pseudo.cpp:29`)给两个伪文件填一个 `0444` 的只读 regular 文件 stat(`st_mode = kProcSIfReg | 0444`,`st_nlink = 1`),让 `stat /proc/meminfo` 行为正常——这是伪文件跟真文件的接口对齐,调用方不需要知道背后是生成出来的。

### 内容生成:procfs_content 的两个 formatter

跟 stat/cmdline 一样,meminfo/cpuinfo 的文本拼接也在 `procfs_content.cpp`(`format_proc_meminfo` 在 `:63`、`format_proc_cpuinfo` 在 `:77`)。`format_proc_meminfo` 拼的是 busybox `free` 会 grep 的那七行:

```cpp
return cinux::fmt::format(buf, cap,
                          "MemTotal: {} kB\n"
                          "MemFree: {} kB\n"
                          "MemAvailable: {} kB\n"
                          "Buffers: {} kB\n"
                          "Cached: {} kB\n"
                          "SwapTotal: {} kB\n"
                          "SwapFree: {} kB\n",
                          total_kb, free_kb, free_kb, 0u, 0u, 0u, 0u);
```

注意 `Buffers / Cached / Swap*` 诚实地写 0,不是瞎编个数——CinuxOS 没跟踪 buffer/cache/swap,记 0 比「编个像样的值」更诚实(busybox `free` 见 0 就显示 0,不会崩)。`MemAvailable` 复用 `free_kb` 也是同一类近似:Linux 真正的 MemAvailable 算法要考虑 reclaimable,这里没那个 accounting,取 free 当可用。`format_proc_cpuinfo` 给每个 CPU 拼一个 `processor / apicid / model name` 块,块间空行分隔——busybox `nproc` 数 `processor :` 行(case-insensitive)就能得到 CPU 数,所以这个排版不只是给人看,是给 `nproc` 的解析路径对齐的。

### 接线:三个地方都得改

光有 `procfs_pseudo.cpp` 不够,meminfo/cpuinfo 要在三处露出来:

1. **`mount()` 建 inode**(`procfs.cpp:341/350`):给 `meminfo_inode_` / `cpuinfo_inode_` 填 `ino` / `type` / `ops` / `mode` / `nlink`,ops 用工厂函数 `procfs_new_meminfo_ops()` / `procfs_new_cpuinfo_ops()`(`procfs_pseudo.cpp:102/105`)拿——这么拐一道是为了把 `ProcMeminfoFileOps` / `ProcCpuinfoFileOps` 的类定义留在 `procfs_pseudo.cpp` 的匿名 namespace 里,不漏给 `procfs.cpp` 这个 TU,避免两个 TU 重复持有同一组子类声明。
2. **根 readdir 列它们**(`procfs.cpp:180/188`):index 2 是 `meminfo`、index 3 是 `cpuinfo`,排在 `.` / `..`(0/1)之后、活 pid 之前(`signal_nth_task_pid(index - 4)`),所以 `ls /proc` 输出顺序是 `meminfo`、`cpuinfo`,然后才是数字 pid 目录。
3. **lookup 解析它们**(`procfs.cpp:407/410` 的 `lookup`、`:463/466` 的 `lookup_child`):裸路径 `/proc/meminfo` 走 `lookup` 的 `strcmp` 直接命中;`vfs_lookup` 一级一级解析走 `lookup_child`(`meminfo` / `cpuinfo` 不是十进制 pid,所以单独 if 提前返,不会进 `parse_pid`)。`lookup_child` 是给 busybox `free` 这类走 `open("/proc/meminfo")` 的路径铺的——`vfs_lookup` 一次解析一个 component,FileSystem 基类的 `lookup_child` 默认返 ENOSYS,不 override 的话 `free` 就报「Function not implemented」(`procfs.hpp:88-93` 的注释明写了这个动机)。

> 静态节点和动态 pid 节点的最大区别不在「内容怎么生成」(都是现场拼文本),而在**inode 身份**:pid 节点靠「pid 有上限 → 定长池」稳住身份;静态节点就一个实例,固定 ino、mount 时建、长存——更接近 DevFS 那张「定长节点表」的做法。所以你把 064 DevFS 的固定节点、这一节的静态伪文件、前面的 pid 池并排看,其实是「虚拟 FS 怎么给 inode 一个稳定身份」这条线在 pid 量纲上的三个特例:全固定(DevFS)→ 单例固定(meminfo/cpuinfo)→ 按整数索引(pid 池)。

> 这里有个诚实的边界要交代:`/proc/version` 和 `/proc/uptime` **还没做**。源码里没有这两个节点的 InodeOps、`mount()` 也没给它们建 inode、readdir 也没列出。`format_proc_cpuinfo` 里虽然拼了 `model name : Cinux x86_64 1.0.0`(`kCpuModel` / `kOsVersion` 来自 `kernel/version.hpp`),但那是塞在 cpuinfo 块里、不是独立的 `/proc/version` 文件;`uptime` 要读时钟、跟 meminfo/cpuinfo 的「读全局变量拼文本」不是同一种活(要算 boot 以来的秒数),做它得先有个稳定的时钟源。两者都留 follow-up,加它们就是在 `procfs_pseudo.cpp` 里再写两个 `InodeOps` 子类、`procfs.cpp` 的 `mount()`/readdir/lookup 三处再挂上——套这套范式即可,但不写不渲染。

## 只露活进程 + 一个 TOCTOU 边界

每条路径的 lookup 都经 `signal_find_task_by_pid` 校验 pid 还活着——进程在 readdir 之后、lookup 之前退出了,lookup 就当没这个 pid(返 NotFound)。这跟 Linux 一致:`/proc` 只露当前活进程。

> 这里有个诚实的边界要交代——但它已经修了。早期版本里 `read` 拿 `signal_find_task_by_pid` 返回裸 `Task*`,该函数在 registry 锁内返回指针、出函数就解锁,read 它的字段时(拼 stat 文本),这个 task 可能已经被另一个核 exit + free——指针悬垂。这个 TOCTOU/UAF 窗口现在由 `signal_snapshot_task`(`signal.hpp:211`)在**字段层**闭合了:它在 registry 锁内把 /proc 要的字段(pid/name/state/ppid/tgid/uid/gid)拷进栈上的 `TaskSnapshot`,出锁后才 format——锁内拷字段、锁外只 format,read 路径不再碰裸指针(DEBT-022 已闭)。`signal.hpp:207-211` 的注释明写「Prefer `signal_snapshot_task()` when you only need fields ... an unlocked `Task*` is a UAF (DEBT-022)」。
>
> `signal_find_task_by_pid` 没被删——它现在只用于 lookup 的**存活校验**(`procfs.cpp:425/:436/:472/:483`,只判 nullptr、不读字段),所以它的 UAF 风险仍在(返回裸指针),但 read 路径已经不再触它。`test_stat_read_dead_pid_is_not_found` 实证「unregister 后读同一 inode → NotFound」,`test_snapshot_task_copies_fields` 实证 snapshot 走的是字段拷贝路径。registry 整体变 RCU-safe 是更后面的事,但 read 路径的字段级 UAF 已经不需要等它了。

## §14 文件门:这一章不能 host 单测

064 DevFS 靠 `CharSink` 注入缝,让核心逻辑(host 能链的那份)可单测。ProcFS 不一样——它**直读 kernel registry**(`signal.hpp` / `process.hpp`),没有注入缝,host 链不了。所以这一章的测试走 kernel harness(QEMU 里 `run_procfs_tests`,12 测),不靠 host 单测。boot 接线(`kprintf`)照例独立 `procfs_init.cpp`,CMake 决定编不编,源码零 `#ifdef`(同 DevFS 的 §14 文件门)。

## 验证

三层验证(没有 host 单测这层)。

**第一层:kernel 测试,ProcFS 机制。** `kernel/test/test_procfs.cpp` 十二例:mount、根 readdir 枚举 pid、lookup `<pid>` 目录、lookup `<pid>/stat` / `<pid>/cmdline` 伪文件、stat 内容格式对、非活 pid → NotFound、release 后读 → NotFound,外加 `test_snapshot_task_copies_fields`(`test_procfs.cpp:348/388`)实证 read 走的是 `signal_snapshot_task` 字段拷贝路径(DEBT-022)。测试自建栈 Task 注册已知 pid(测试内核主线程不进 registry,不能依赖残留状态),确定性验证。

**第二层:boot 冒烟。** `make run` 起 QEMU,看 `[PROCFS] mounted at /proc`(`procfs::init()` 在 `devfs::init()` 之后挂)。`ls /proc` 见到 pid,`cat /proc/<pid>/stat` 读到 `pid (name) state ppid ...`。

**第三层:全量回归。** 改了公共头 `signal.hpp`(加 `signal_nth_task_pid` 和 `signal_snapshot_task`),push 前全量编(含 host)。`run-kernel-test-all` 两腿各 **998 passed / 0 failed**(986 基线 + 12 procfs)。

## 这章没做的

- **静态节点**:`/proc/meminfo` 和 `/proc/cpuinfo` 已经做了(机制见上面「静态伪文件节点」一节);`/proc/version` 和 `/proc/uptime` 这两个**还没做**——源码里没有它们的 InodeOps、`mount()` 也没建 inode,留 follow-up。`uptime` 要算 boot 以来的秒数,得先有个稳定时钟源,跟 meminfo/cpuinfo 那种「读全局变量拼文本」不是同一种活。
- **更多每进程伪文件**:`/proc/<pid>/maps`(地址空间)、`/proc/<pid>/fd`(打开文件)、`/proc/<pid>/status` 没做。
- **完整 Linux `/proc/<pid>/stat`**:Linux 的 stat 有 52 个字段(含 per-task accounting),CinuxOS 没有那些 accounting,只做简化子集。
- **真 argv**:Task 不存 argv,`cmdline` 只暴露 comm(名字)。真 NUL 分隔的 argv 要 Task 存 argv,留 follow-up。
- **registry TOCTOU**:find 后 unlock、read 时 task 可能已 free 的窗口,真修要 registry RCU-safe(后续工程债:registry RCU 化)。
- **host 可测**:ProcFS 直读 registry 无注入缝,核心逻辑 host 链不了。要 host 可测得抽个 `TaskInfoProvider` 接口(同 DevFS CharSink 缝),留 follow-up。

## 小结

- ProcFS 照 DevFS 范式(`FileSystem` 子类 + `InodeOps` 子类 + boot `_init.cpp`),用到进程自省:`/proc` 根枚举活 pid,`/proc/<pid>/{stat,cmdline}` 现场生成文本。
- 新问题是**动态的根**:DevFS 根是定长节点表,ProcFS 根是当前活 pid。靠 PID 有上限(256),预备定长 inode 池按 pid 索引,每 pid 一个稳定 inode——顺带 SMP 安全(无 scratch 复用竞态)。
- readdir 用 `signal_nth_task_pid`(「第 n 个」accessor)而非快照数组,避开栈上大数组超 `-Wframe-larger-than`;纯增量,不改 registry 写路径。
- 伪文件 read 时 `signal_snapshot_task` 在 registry 锁内把字段拷进栈上 `TaskSnapshot`、出锁后 `format_proc_stat`/`format_proc_cmdline` 现场拼文本;每条 lookup 校验 pid 活着(用 `signal_find_task_by_pid` 判 nullptr),只露活进程。
- 诚实边界:read 路径曾经有 find-after-unlock 的 TOCTOU/UAF 窗口,现由 `signal_snapshot_task`(DEBT-022)在字段层闭合——锁内拷字段、锁外只 format;`signal_find_task_by_pid` 仍返回裸指针,仅留作 lookup 存活校验用,registry 整体 RCU-safe 仍是后续工程债。ProcFS 直读 registry 无注入缝,不能 host 单测,走 kernel harness;`/proc/version`/`uptime`、maps/fd/真 argv 留 follow-up。
