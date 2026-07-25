---
title: Lab 013 · ProcFS TOCTOU snapshot 修复验证
---

# Lab 013 · ProcFS TOCTOU snapshot 修复验证

> 对应 `document/book/08-filesystem/013/`。验证档 **B 档**:这一章是个聚焦修复——把 ProcFS read 的「锁内拿指针、锁外解引用」TOCTOU 改成「锁内拷值(snapshot)」。验证靠新增 snapshot 单测 + 既有 stat/cmdline 读测自动验 + 两腿不回归。

## 目标

确认五件事:

1. **TOCTOU 根因坐实**:`find_by_pid` 锁内拿指针 unlock 返回,「task 永不释放」前提失效(task 真 delete)→ SMP UAF;
2. **TaskSnapshot POD**(pid/state/ppid/tgid/uid/gid + name 字节缓冲);
3. **signal_snapshot_task 锁内拷值**(指针不逃出锁);
4. **ProcFS read 改吃 snapshot**(format 签名 Task*→TaskSnapshot&);
5. **find_by_pid 注释订正**(safe→WARNING),没删(还有别的调用方)。

## 步骤

### 1. snapshot 单测

```bash
cmake --build build --target run-kernel-test 2>&1 | grep -iE "snapshot" | head
```

应看到 `test_snapshot_task_copies_fields` PASS——验 `signal_snapshot_task` 锁内把字段拷进 snapshot(pid/state/ppid/tgid/uid/gid + name)对。既有 stat/cmdline 读测(`test_procfs` 那批)现在走 snapshot 路径,自动验端到端。

### 2. TaskSnapshot POD

```bash
sed -n '25,42p' kernel/proc/task_snapshot.hpp
```

应看到 `kTaskNameMax = 16` + `struct TaskSnapshot`(pid/state/ppid/tgid/uid/gid + `name[kTaskNameMax]` 字节缓冲)。这是个**自包含 POD**——锁释放后跟原 task 死活脱钩,name 是字节拷贝不是指针。

### 3. signal_snapshot_task 锁内拷值

```bash
sed -n '96,120p' kernel/proc/signal.cpp
```

应看到 `signal_snapshot_task(pid, out)`:`g_registry_lock` 下找到 task、字段拷进 snapshot(name 逐字节拷截断 `kTaskNameMax-1`)、返 bool。**指针不逃出锁**——这是闭 TOCTOU 的关键(旧的 find_by_pid 是锁内找指针、unlock 返指针,锁外解引用)。

### 4. ProcFS read 改吃 snapshot

```bash
sed -n '38,48p' kernel/fs/procfs_content.hpp
```

应看到 `format_proc_stat(const TaskSnapshot& s, ...)` / `format_proc_cmdline(const TaskSnapshot&, ...)`——签名从 `const Task*` 改成 `const TaskSnapshot&`。ProcFS 的 stat/cmdline read 不再 `find_by_pid` 拿裸指针,改 `TaskSnapshot snap; signal_snapshot_task(pid, snap); format(snap, ...)`。

### 5. find_by_pid 注释订正(没删)

```bash
sed -n '200,212p' kernel/proc/signal.hpp
```

应看到 `find_task_by_pid` 的注释从「safe while never freed」改成 **WARNING**:指针仅持锁期有效,锁外解引用是 UAF,要字段走 `signal_snapshot_task`。`find_by_pid` **没删没改签名**——它还有别的调用方(test_clone/test_fork_exec/sys_pgrp/ProcFS lookup 活性门),那些只判 nullptr 或容忍 Zombie/Dead,不锁外解引用多字段。

两腿汇总:

```bash
cmake --build build --target run-kernel-test-all 2>&1 | grep -E "Tests: [0-9]{4} passed" | tail -1
```

应看到单核 + `-smp 2` 两腿各 `1020 passed, 0 failed`。

## 验收清单

- [ ] `test_snapshot_task_copies_fields` PASS;既有 stat/cmdline 读测(snapshot 路径)全绿。
- [ ] `task_snapshot.hpp:32` `TaskSnapshot` POD(name 字节缓冲 `kTaskNameMax=16`,不是指针)。
- [ ] `signal.cpp:96` `signal_snapshot_task` 锁内拷值(指针不逃出锁)。
- [ ] `procfs_content.hpp:40` format 签名 `const TaskSnapshot&`(原 `const Task*`)。
- [ ] `signal.hpp:204` find_by_pid 注释 WARNING(原 safe),没删没改签名。
- [ ] 两腿 1020/0。

## 别做这些

- **别**以为「task 永不释放」还是真——早先的 task 释放修复让 task 经 exit→reap_deferred→delete 真释放了。所有依赖「永不释放」的锁外解引用(ProcFS read 那个)在 SMP 下是真 UAF,经 sys_open/sys_read 敞开窗口。
- **别**删 `find_task_by_pid` 或改它签名——它还有别的调用方(test_clone/test_fork_exec/sys_pgrp/ProcFS 活性门),那些不锁外解引用多字段。只订正注释 + 加 snapshot 替代。
- **别**用拷 name 指针代替拷字节——现在 name 是 static storage 不悬,但这次修复的教训就是「别信赖过时的不释放契约」。今天 static,明天可能堆存。按字节拷(16 字节栈代价)把这个未来风险也堵了。
- **别**以为 registry TOCTOU 全闭了——这一章只闭了 ProcFS read(经 syscall 敞开窗口的最危险那条)。killpg(仍 snapshot 指针,容忍 Zombie/Dead)、sys_pgrp(返裸指针)是同族债,范围外。全闭要 refcount/RCU(registry 引用计数级别大改,长期)。
- **别**把 snapshot 当 RCU——snapshot 是「锁内拷值、锁外用副本」,零接口变更的收敛修;RCU 是 registry 引用计数让指针真安全。snapshot 闭一条路,RCU 闭全部。这章是前者。
