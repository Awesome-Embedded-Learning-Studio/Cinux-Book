---
title: 04 · 收尾:TOCTOU 边界、文件门、验证与小结
---

# 收尾:TOCTOU 边界、文件门、验证与小结

## 只露活进程 + 一个 TOCTOU 边界

每条路径的 lookup 都经 `signal_find_task_by_pid` 校验 pid 还活着——进程在 readdir 之后、lookup 之前退出了,lookup 就当没这个 pid(返 NotFound)。这跟 Linux 一致:`/proc` 只露当前活进程。

> 这里有个诚实的边界要交代——但它已经修了。早期版本里 `read` 拿 `signal_find_task_by_pid` 返回裸 `Task*`,该函数在 registry 锁内返回指针、出函数就解锁,read 它的字段时(拼 stat 文本),这个 task 可能已经被另一个核 exit + free——指针悬垂。这个 TOCTOU/UAF 窗口现在由 `signal_snapshot_task`(`signal.hpp:211`)在**字段层**闭合了:它在 registry 锁内把 /proc 要的字段(pid/name/state/ppid/tgid/uid/gid)拷进栈上的 `TaskSnapshot`,出锁后才 format——锁内拷字段、锁外只 format,read 路径不再碰裸指针(DEBT-022 已闭)。`signal.hpp:207-211` 的注释明写「Prefer `signal_snapshot_task()` when you only need fields ... an unlocked `Task*` is a UAF (DEBT-022)」。
>
> `signal_find_task_by_pid` 没被删——它现在只用于 lookup 的**存活校验**(`procfs.cpp:425/:436/:472/:483`,只判 nullptr、不读字段),所以它的 UAF 风险仍在(返回裸指针),但 read 路径已经不再触它。`test_stat_read_dead_pid_is_not_found` 实证「unregister 后读同一 inode → NotFound」,`test_snapshot_task_copies_fields` 实证 snapshot 走的是字段拷贝路径。registry 整体变 RCU-safe 是更后面的事,但 read 路径的字段级 UAF 已经不需要等它了。

## §14 文件门:这一章不能 host 单测

010 DevFS 靠 `CharSink` 注入缝,让核心逻辑(host 能链的那份)可单测。ProcFS 不一样——它**直读 kernel registry**(`signal.hpp` / `process.hpp`),没有注入缝,host 链不了。所以这一章的测试走 kernel harness(QEMU 里 `run_procfs_tests`,12 测),不靠 host 单测。boot 接线(`kprintf`)照例独立 `procfs_init.cpp`,CMake 决定编不编,源码零 `#ifdef`(同 DevFS 的 §14 文件门)。

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
