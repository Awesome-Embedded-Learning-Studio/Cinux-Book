---
title: Lab 011 · ProcFS 验证
---

# Lab 011 · ProcFS 验证

> 对应 `document/book/08-filesystem/011/`。验证档 **A 档**:这一章交付的是 `/proc` 真挂上、`ls /proc` 见 pid、`cat /proc/<pid>/stat` 读到进程信息。注意 ProcFS **没有 host 单测**——它直读 kernel registry(无注入缝,不像 DevFS 的 CharSink),所以核心逻辑走 kernel harness 测;boot 真 `/proc` 靠 `make run` 冒烟。

## 目标

确认六件事:

1. **ProcFS 照 DevFS 范式**(`FileSystem` 子类 + `InodeOps` 子类 + boot `_init.cpp`),换个场景用到进程自省;
2. **定长 pid 索引 inode 池**(PID_MAX=256,每 pid 一个稳定 inode,SMP 安全);
3. **readdir 用 nth accessor**(`signal_nth_task_pid`,避栈数组超 frame 限);
4. **伪文件现场生成文本**(`/proc/<pid>/stat` = pid (name) state ppid ...);
5. **只露活进程**(每条 lookup 校验 pid 活着);
6. **boot 真挂 /proc**:`make run` 见 `[PROCFS] mounted`,`ls /proc` 见 pid。

## 步骤

### 1. kernel 测试:ProcFS 机制(无 host 单测)

ProcFS 直读 kernel registry,host 链不了,测试走 kernel harness:

```bash
cmake --build build --target run-kernel-test 2>&1 | grep -iE "procfs" | head
```

应看到 `test_procfs::test_mount_and_root` 等十一项 PASS:mount、根 readdir 枚举 pid、lookup `<pid>` 目录、lookup `<pid>/stat`/`cmdline`、stat 内容格式、非活 pid → NotFound、release 后读 → NotFound。测试自建栈 Task 注册已知 pid(测试主线程不进 registry),确定性验证。

> 为什么没 host 单测?DevFS 靠 `CharSink` 注入缝让核心 host 可测;ProcFS 直读 `signal.hpp`/`process.hpp` 的 registry,没有这层缝,host 一链就缺符号。所以核心逻辑只能在内核里测。要 host 可测得抽个 `TaskInfoProvider` 接口(同 DevFS 的 CharSink 思路),留 follow-up。

### 2. ProcFs 范式 + 定长 inode 池

```bash
sed -n '45,70p' kernel/fs/procfs.hpp
```

应看到 `kProcPidMax = 256` + `pid_dir_inodes_[257]` / `stat_inodes_[257]` / `cmdline_inodes_[257]` 定长池(pids 1..256,slot 0 留空),`ino = pid`、`fs_private = this`。文件头注释会讲为什么:VFS 的 close 只删 File 不删 Inode,所以每个 lookup 返回的 inode 必须由 ProcFS 长存;pid 有上限(256),预备定长池按 pid 索引,每 pid 一个稳定 inode——顺带 SMP 安全(并发 lookup 不同 pid 不竞态,无 scratch 复用覆写)。`static_assert(kProcPidMax == PidAllocator::PID_MAX)` 把池上界跟 allocator 锁死。

### 3. readdir:nth accessor(避栈数组)

```bash
sed -n '89,110p' kernel/proc/signal.cpp
```

应看到 `signal_nth_task_pid(n, out_pid)`:registry 锁内走到第 n 个 task,写出 pid;走不到返 false。这是纯增量(不改 register/unregister/find_by_pid)。再看 readdir 怎么用:

```bash
grep -n "signal_nth_task_pid\|readdir" kernel/fs/procfs.cpp | head
```

`ProcRootDirOps::readdir` 索引 0/1 = `.`/`..`,索引 i≥2 = 第 (i-2) 个活 pid。用「第 n 个」而非「快照整个 pids[] 数组」,是为了不在内核栈上放 `int pids[257]`(1028 B,超 `-Wframe-larger-than=1024`)。代价是 O(tasks²),tasks ≤ 256 可忽略。

### 4. 伪文件:现场生成文本

```bash
sed -n '77,105p' kernel/fs/procfs_content.cpp
```

应看到 `format_proc_stat(t, buf, cap)`:拼 `pid (name) state ppid tgid uid gid\n`(Linux `/proc/<pid>/stat` 的简化子集;state 是单字符 `task_state_char`,R/S/T/Z/X)。`format_proc_cmdline` 是 task 名字 + NUL(Task 不存 argv,best-effort 暴露 comm)。read 时 `signal_find_task_by_pid` 取 Task → 生成到栈 buffer → 按 offset 拷贝。

### 5. 只露活进程

```bash
grep -n "signal_find_task_by_pid\|NotFound" kernel/fs/procfs.cpp | head
```

应看到每条路径的 lookup 都经 `signal_find_task_by_pid` 校验 pid 还活着——非活 pid → NotFound。对齐 Linux:`/proc` 只露当前活进程。进程在 readdir 后、lookup 前退出,就当没这个 pid。

### 6. boot 真挂 /proc

```bash
grep -n "procfs::init\|PROCFS" kernel/proc/init.cpp kernel/fs/procfs_init.cpp | head
```

应看到 `procfs::init()` 在 `devfs::init()` 之后调(init.cpp),它 mount ProcFs + `vfs_mount_add("/proc", ...)`。冒烟(test kernel 不走 boot,要起真内核):

```bash
cmake --build build --target run
```

应看到 boot 序列:`[VFS] ext2 mounted at /` → `[DEVFS] mounted at /dev (4 nodes)` → `[PROCFS] mounted at /proc`。进 shell 后:

- `ls /proc` 见到 pid 数字(当前进程们);
- `cat /proc/<某个 pid>/stat` 读到 `pid (name) state ppid ...`。

两腿汇总:

```bash
cmake --build build --target run-kernel-test-all 2>&1 | grep -E "Tests: 9[0-9][0-9] passed" | tail -1
```

应看到单核 + `-smp 2` 两腿各 `997 passed, 0 failed`(986 基线 + 11 procfs)。

## 验收清单

- [ ] `run-kernel-test` 见 `test_procfs` 十一项 PASS(无 host 单测——ProcFS 直读 registry)。
- [ ] `procfs.hpp:51` `kProcPidMax=256` + `pid_dir/stat/cmdline_inodes_[257]` 定长池,`ino=pid`,`static_assert` 锁 PID_MAX。
- [ ] `signal.cpp:89` `signal_nth_task_pid`(纯增量 nth accessor);`procfs.cpp` readdir 索引 i≥2 = 第(i-2)个活 pid。
- [ ] `procfs_content.cpp:77` `format_proc_stat`(pid name state ppid ...)+ `format_proc_cmdline`(name+NUL)。
- [ ] 每条 lookup 经 `signal_find_task_by_pid` 校验存活,非活 pid → NotFound。
- [ ] `make run` 见 `[PROCFS] mounted at /proc`;`ls /proc` 见 pid;两腿 997/0。

## 别做这些

- **别**找 host 单测——ProcFS 直读 kernel registry(signal.hpp/process.hpp),没注入缝,host 链不了。核心逻辑在 kernel harness 测。要 host 可测得抽 `TaskInfoProvider` 接口,留 follow-up。
- **别**以为 `/proc` 根是固定节点——它是**当前活 pid**(动态)。DevFS 的根是写死的 null/zero/console;ProcFS 的根随进程创建/退出变化。靠定长 inode 池(PID 有上限)给每 pid 稳定 inode。
- **别**用「快照整个 pids 数组」做 readdir——栈上 `int pids[257]` 超 `-Wframe-larger-than=1024`(production -Werror)。用 nth accessor(`signal_nth_task_pid`)锁内走到第 n 个,无栈数组。
- **别**指望 `/proc/<pid>/stat` 是完整 Linux 52 字段——CinuxOS 没 per-task accounting,只做简化子集(pid name state ppid tgid uid gid)。maps/fd/status/version/meminfo 都没做,留 follow-up。
- **别**以为读 `/proc/<pid>/stat` 完全无竞态——find_task_by_pid 返指针后解锁,read 字段时 task 可能已 exit+free(TOCTOU 窗口)。窗口极小、hobby OS 可接受,真修要 registry RCU-safe(后续工程债:registry RCU 化)。
- **别**以为 `cmdline` 是真 argv——Task 不存 argv,只暴露 comm(名字)。真 NUL 分隔 argv 要 Task 存 argv,留 follow-up。
