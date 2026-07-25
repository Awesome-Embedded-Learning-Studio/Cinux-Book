---
title: 01 · 导引:范式与点亮什么
---

# 导引:范式与点亮什么

> A 档:punchline 是 `/proc` 真挂上、`ls /proc` 见到 pid、`cat /proc/<pid>/stat` 读到进程信息。这一章真正要讲的是「**虚拟 FS 的根是动态的**」时怎么处理——DevFS 的根是一张固定节点表(null/zero/console 写死),ProcFS 的根是**当前活进程的 pid**,进程随时在创建和退出,这个目录是活的。还有「读伪文件 = 现场从内核结构生成文本」这套做法。一条诚实的边界先说在前头:这是进程自省的第一刀——做 `/proc` 根枚举 pid + `/proc/<pid>/{stat,cmdline}`,外加 `/proc/meminfo`/`/proc/cpuinfo` 两个静态节点(都从 `g_pmm`/`g_acpi_info` 现场生成);`/proc/version`/`uptime` 静态节点和 `maps`/`fd`/`status` 每进程伪文件还不做,留 follow-up。

## 这章咱们要点亮什么

1. **同一个范式换个场景**:DevFS 的「`FileSystem` 子类 + `InodeOps` 子类 + boot `_init.cpp`」范式直接套到进程自省。范式立一次,多处复用。
2. **虚拟 FS 的根是动态的时候怎么办**:DevFS 根是定长节点表,ProcFS 根是当前活 pid——靠定长 inode 池(PID 有上限)给每个 pid 一个稳定 inode,不靠单个 scratch 复用。
3. **readdir 枚举活进程**:给进程 registry 加一个「走到第 n 个」的 accessor(`signal_nth_task_pid`),readdir 按它列 pid。用「第 n 个」而不是「快照整个数组」,是为了不撑爆内核栈帧。
4. **伪文件 = 现场生成文本**:`/proc/<pid>/stat` 读的时候,从进程表取出那个 Task,现场拼成 `pid (name) state ppid ...` 文本返回。
5. **只露活进程**:每条路径 lookup 都校验 pid 还活着——进程在 readdir 和 lookup 之间退出了,就当作没这个 pid(对齐 Linux)。

## 同一个范式:ProcFs 是另一个虚拟 FS

DevFS 那章立的范式,这里照搬:`ProcFs : FileSystem`,设备/伪文件行为写成 `InodeOps` 的匿名 namespace 子类,boot 接线(`procfs::init()` 挂 `/proc`)单独放 `procfs_init.cpp`。所以这一章不重复讲范式本身(看 010),只讲 ProcFS 比 DevFS 多出来的两个新问题:**动态的根** 和 **伪文件的内容生成**。
