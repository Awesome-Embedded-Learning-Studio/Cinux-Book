---
title: Lab 079 · VFS 收尾验证:目录项缓存与文件锁
---

# Lab 079 · VFS 收尾验证:目录项缓存与文件锁

> 对应 `document/book/08-filesystem/079-vfs-finale-dentry-flock.md`。验证档 **A 档**:punchline 是 `run-kernel-test` 里 `run_dentry_tests` + `run_flock_tests` 两条机制测都过——dentry cache 的命中/未命中/失效,flock 的共享并发/排他互斥/`LOCK_NB` 冲突返 `EAGAIN`。两条机制按 Linux 语义答了,VFS 的「快」(缓存)和「对」(锁)就齐了。

## 目标

确认四件事:

1. **dentry cache 命中跳过底层**:同一段 `(父 inode, 名字)` 第二次查走缓存、不再问文件系统;命中返回的 child 是新引用(调方 unref);
2. **dentry cache 失效正确**:`unlink`/`rmdir`/`rename` 之后对应条目失效,下次查重新走底层;
3. **flock 共享 vs 排他**:多个 `LOCK_SH` 能同时持有;`LOCK_EX` 互斥所有其他锁;
4. **flock 非阻塞冲突**:`LOCK_EX|LOCK_NB` 撞上已持锁返 `EAGAIN`,不带 `LOCK_NB` 才阻塞。

## 步骤

### 1. 编得过 + 测试注册进 harness

dentry/flock 源是无条件编;机制测挂在 `big_kernel_test` 里。先确认两份都绿、测试也注册了:

```bash
cmake --build build -j$(nproc) 2>&1 | grep -iE 'dentry|flock|Built target big_kernel_test' | head
# 确认 run_dentry_tests / run_flock_tests 在 main_test 里被调:
grep -nE 'run_dentry_tests|run_flock_tests' kernel/test/main_test.cpp
```

应看到 `dentry.cpp`/`file_lock.cpp`/`sys_flock.cpp` 都编了、无 error,且 main_test.cpp 里有 `run_dentry_tests();` + `run_flock_tests();` 两处调用。

### 2.(A 档 punchline)跑机制测看 PASS

```bash
cmake --build build --target run-kernel-test 2>&1 | grep -aE 'dentry|flock|Tests:' | head -20
```

串口上应看到 dentry 与 flock 的测试用例 `[PASS]`,最后是 `Tests: 1080 passed, 0 failed`(本章比 077 多出 dentry + flock 两组用例)。

### 3. 验 dentry cache 三种行为

机制测覆盖三条,对得上章节讲的:

- **未命中→填缓存**:第一次 `lookup(parent, "child")` 返 `nullptr`(未命中),触发底层 `lookup_child`,拿到后 `add(...)` 填回;`DentryCache::count()` 加 1。
- **命中→返回新引用**:第二次 `lookup(parent, "child")` 直接命中,返回的 child 与第一次是同一个 `Inode*`,但**引用计数多了一**(调用方要 `inode_unref` 才平衡)。测里会断言「命中前后是同一指针」+「count 不再涨」。
- **失效→重新走底层**:`invalidate(parent, "child")` 后,`count` 减 1,下一次 `lookup` 又返 `nullptr`(回到未命中)。

要肉眼确认,在测试运行时把 [dentry.cpp](kernel/fs/dentry.cpp) 的 `add`/`invalidate` 临时加一行 `kprintf` 打印 count,重跑就能看到「miss→add(count+1)→hit(count 不变)→invalidate(count−1)」的节奏。验完记得删掉打印。

### 4. 验 flock 的冲突矩阵

机制测把冲突规则跑了一遍,重点看这几条断言:

- `LOCK_SH` + `LOCK_SH`(同 inode,不同 owner)→ 都成功(共享不互斥);
- `LOCK_SH` + `LOCK_EX` → 后者冲突;
- `LOCK_EX` + 任何 → 都冲突;
- 冲突 + `LOCK_NB` → 立刻返 `EAGAIN`(`-1` 的负数 errno);
- 冲突 + 不带 `LOCK_NB` → 阻塞(测试 harness 单线程,这条用「直接构造已锁状态 + 验逻辑判定」模拟,不真睡);
- `LOCK_UN` / `close(fd)` → 释放该 owner 在该 inode 上的锁,等的人能拿。

> flock 的「阻塞唤醒」真要验「睡→醒」需要多任务并发(harness 单线程模拟不了真实的 `schedule_blocked`→`wake_all` 往返)。机制测验的是**判定逻辑**(给一个已锁状态,新来的 lock 该不该冲突、返不返 EAGAIN),这部分单线程能覆盖。真并发下的唤醒竞态在隔壁正确性弧的 race-detect 里。

## 范围与边界

- 本章的 dentry cache **无 LRU 淘汰**(条目只增不减,除 `invalidate`)——测里不验淘汰,因为就没有。要加有界缓存是后续工程。
- flock 是**建议锁**且**按 task 粗粒度**释放(`close` 释放该任务在该 inode 上全部锁,不区分 `dup` 共享一个 open-file-description)——这是简化,测里不验 `dup` 共享语义。
- `sys_mount` 的块设备挂载链(`mount -t` 用 block_registry)本轮没回迁(依赖 GCC 自举弧的 `sys_mount`),所以 lab 不验「挂一块新盘」。dentry/flock 在已有的 ext2/procfs/devfs 上验,不依赖新挂载。
