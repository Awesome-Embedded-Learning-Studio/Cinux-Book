---
title: 01 · ProcFS 的 TOCTOU:锁内拷值,指针不逃出锁
---

# ProcFS 的 TOCTOU:锁内拷值,指针不逃出锁

> 011 立 ProcFS 的时候,诚实交代过一个窗口:`/proc/<pid>/stat` 读的时候,`signal_find_task_by_pid` 在 registry 锁内找到 Task 指针、**释放锁返回**,然后 `format_proc_stat` 在锁外解引用那个指针读字段。当时说「task 永不释放,窗口极小,hobby OS 可接受,真修等 registry RCU 化」。这一章兑现那笔债——但用的是比 RCU 轻得多的办法:**锁内把字段拷成一个自包含的 snapshot(POD),锁外用的是 snapshot 不是指针**,指针一出锁就没用了。根因比 011 当时以为的更严重:那个「task 永不释放」的前提**早就失效了**(Task 现在会真 delete),所以这个窗口不是「极小」,是「SMP 下真 UAF」。
>
> B 档:这一章是个聚焦修复,没有新能力。验证靠新增的 snapshot 单测 + 既有 stat/cmdline 读测(自动验 snapshot 端到端)+ 两腿不回归。一条诚实的边界:这一章只闭了 ProcFS read 这一条路;registry 还有别的「锁内拿指针、锁外用」同族窗口(killpg、sys_pgrp),那些靠容忍 Zombie/Dead 兜着,全闭要等 refcount/RCU(长期)。

## 这章咱们要点亮什么

1. **TOCTOU(检查-使用竞态)**:锁内检查(找到 task)、锁外使用(读字段),中间窗口里那个 task 可能被另一个核释放了 → use-after-free。
2. **根因比想象严重**:「task 永不释放」的前提失效了——Task 现在经 `exit → reap_deferred → delete` 真释放。所以 ProcFS read 那个窗口在 SMP 下是真 UAF,不是「极小」。
3. **snapshot 法**:锁内把要的字段拷进一个 POD 结构(指针不逃出锁),锁外用 snapshot。比 RCU 轻(零接口变更),够闭这一条路。
4. **name 拷字节而非指针**:教训是「别信赖过时的不释放契约」——就算现在 name 是 static storage 指针不悬,也按字节拷,防御未来 name 改堆存。

## 根因:一个失效的前提 + 一个敞开的窗口

先看 011 那个窗口为什么从「极小」变成「真 UAF」。

`/proc/<pid>/stat` 的 read 路径,原来调 `signal_find_task_by_pid(pid)` 拿 `Task*`。这个函数在 `g_registry_lock` 下找到指针、**释放锁、返回指针**;然后 `format_proc_stat(t, ...)` 在锁外读 `t->pid / name / state / ppid / tgid / uid / gid` 七个字段。

011 当时(和 signal.cpp 的注释)都写着「tasks 永不释放,安全」。可这个前提**已经失效**了:早先的 task 释放修复改了这点——Task 现在经 `exit_current → signal_unregister_task → reap_deferred → delete t` **真释放**。于是 SMP(-smp 2)下:

- CPU0:`cat /proc/<pid>/stat` → `find_by_pid` 拿到 `t` → unlock;
- CPU1:那个 task `exit_current` → `reap_deferred` → `delete t`;
- CPU0:`format_proc_stat(t, ...)` 解引用已 free 的 Task → **UAF**。

`/proc` 经 sys_open/sys_read 可达,这两条 syscall 不关抢占/IRQ——窗口完全敞开,不是「极小」。这是个真 bug,不是一个「理论上的窗口」。

> 教训:「永不释放」这种契约,一旦底层改了(这里 task 开始真 delete),所有依赖它的代码都静默变错。signal.cpp 的注释白纸黑字写着「safe while never freed」,可前提没了它就成了谎言。审核时挖出来的,正是这种「注释声称安全、前提已失效」的潜伏 bug。

## 修法:锁内拷值,指针不逃出锁

对齐 `killpg` 的「锁内 snapshot」范式,但 killpg snapshot 的是指针(仍依赖不释放),ProcFS 要更稳——**snapshot 值**。

立一个 POD 结构 `TaskSnapshot`(`task_snapshot.hpp:32`):

```cpp
struct TaskSnapshot {
    int       pid;
    TaskState state;
    int       ppid;
    int       tgid;
    uint32_t  uid;
    uint32_t  gid;
    char      name[kTaskNameMax];   // kTaskNameMax=16,字节缓冲,不是指针
};
```

加一个 accessor `signal_snapshot_task(pid, TaskSnapshot& out)`(`signal.hpp:211`):在 `g_registry_lock` 下找到 task、把字段**拷进 snapshot**(name 逐字节拷、截断到 `kTaskNameMax-1`,对齐 Linux `TASK_COMM_LEN`),返回 bool(找到)。锁一释放,snapshot 是个自包含的 POD,跟原 task 的死活彻底脱钩。

ProcFS 的 read 改走 snapshot:`ProcStatFileOps::read` / `ProcCmdlineFileOps::read` 不再 `find_by_pid` 拿裸指针,改 `TaskSnapshot snap; signal_snapshot_task(pid, snap)`;`format_proc_stat` / `format_proc_cmdline` 的签名从 `const Task*` 改成 `const TaskSnapshot&`(`procfs_content.hpp:59,65`)。

> name 为什么按**字节**拷,而不是拷那个 `const char*` 指针?现在 `Task::name` 是「static storage, not owned」(全字面量),指针其实不悬。但这次修复的教训恰恰是「别信赖过时的不释放契约」——今天 name 是 static,明天可能改堆存(动态建 task 名),到时候拷指针就悬了。多花 16 字节栈按字节拷,把这个未来风险也堵了。代价极小,防御明确。

## 边界:这一章只闭了 ProcFS read 这一条

诚实交代,registry 的 TOCTOU 不止这一处,这一章只闭了 ProcFS read:

- `find_task_by_pid` 还在(没删/没改签名)——它还有别的调用方(test_clone、test_fork_exec、sys_pgrp、ProcFS lookup 的活性门),那些只判 nullptr 或容忍 Zombie/Dead,不锁外解引用多字段。这一章只**订正了它的注释**(从「safe while never freed」改成 WARNING:指针仅持锁期有效,锁外解引用是 UAF,要字段走 `signal_snapshot_task`)。
- `killpg` 仍 snapshot 指针(靠 `signal_send` 容忍 Zombie/Dead 兜着);
- `sys_pgrp` 返裸指针给调用方——潜在同族债(范围外,没修)。

**registry TOCTOU 全闭要等 refcount/RCU**(Task 引用计数,真正的 RCU-safe registry)。那是 registry 引用计数级别的大改;snapshot 是 F6 范畴内、零接口变更的收敛修——闭掉最危险的(经 syscall 敞开窗口的 ProcFS read),其余靠容忍兜着,记着。

## 验证

- 全量编译绿(改公共头 `signal.hpp`/`task_snapshot.hpp` 触发大重建,所有消费者过)。
- `run-kernel-test-all` 两腿各 **1020 passed / 0 failed**(单核 + -smp 2)。新增 `test_snapshot_task_copies_fields`(验锁内拷字段对);既有 stat/cmdline 读测自动验 snapshot 端到端(它们现在走 snapshot 路径)。
- host `ctest` 69/69(public 头没破 mock)。

## 小结

- 011 的 ProcFS TOCTOU 窗口,根因比想象严重:「task 永不释放」前提失效(task 开始真 delete),SMP 下是真 UAF(经 sys_open/sys_read 敞开窗口)。
- snapshot 法:`TaskSnapshot` POD(pid/state/ppid/tgid/uid/gid + name 字节缓冲),`signal_snapshot_task` 锁内拷值;ProcFS read 改吃 snapshot,`format` 签名 `Task*`→`TaskSnapshot&`。指针不逃出锁,name 按字节拷(防御未来改堆存)。
- 教训:别信赖「永不释放」这种过时契约——底层一改,所有依赖它的代码静默变错。signal.cpp 的注释从「safe」改成 WARNING。
- 边界:只闭了 ProcFS read;killpg/sys_pgrp 的同族窗口靠容忍兜着,registry TOCTOU 全闭要等 refcount/RCU(长期)。`find_task_by_pid` 没删(还有别的调用方),只订正注释。
