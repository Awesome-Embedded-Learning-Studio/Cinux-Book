---
title: 03 · 伪文件与 procfs_pseudo:读时现场生成文本
---

# 伪文件与 procfs_pseudo:读时现场生成文本

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

> 静态节点和动态 pid 节点的最大区别不在「内容怎么生成」(都是现场拼文本),而在**inode 身份**:pid 节点靠「pid 有上限 → 定长池」稳住身份;静态节点就一个实例,固定 ino、mount 时建、长存——更接近 DevFS 那张「定长节点表」的做法。所以你把 010 DevFS 的固定节点、这一节的静态伪文件、前面的 pid 池并排看,其实是「虚拟 FS 怎么给 inode 一个稳定身份」这条线在 pid 量纲上的三个特例:全固定(DevFS)→ 单例固定(meminfo/cpuinfo)→ 按整数索引(pid 池)。

> 这里有个诚实的边界要交代:`/proc/version` 和 `/proc/uptime` **还没做**。源码里没有这两个节点的 InodeOps、`mount()` 也没给它们建 inode、readdir 也没列出。`format_proc_cpuinfo` 里虽然拼了 `model name : Cinux x86_64 1.0.0`(`kCpuModel` / `kOsVersion` 来自 `kernel/version.hpp`),但那是塞在 cpuinfo 块里、不是独立的 `/proc/version` 文件;`uptime` 要读时钟、跟 meminfo/cpuinfo 的「读全局变量拼文本」不是同一种活(要算 boot 以来的秒数),做它得先有个稳定的时钟源。两者都留 follow-up,加它们就是在 `procfs_pseudo.cpp` 里再写两个 `InodeOps` 子类、`procfs.cpp` 的 `mount()`/readdir/lookup 三处再挂上——套这套范式即可,但不写不渲染。
