---
title: 04 · 主线二:文件锁 flock——让进程协商谁先写
---

# 主线二:文件锁 flock——让进程协商谁先写

### 建议锁是什么意思

flock(2) 给的是**建议锁(advisory lock)**:内核只记录「这个 inode 被这个任务以什么模式锁了」,**不强制**阻止别人读写。一个进程不调 `flock` 直接写,内核照样让它写——锁只对**也调了 flock 的进程**有意义。

这跟「强制锁」(mandatory lock,读写操作本身会被内核拦)不一样。Linux 的 flock 是建议锁,进程之间靠自觉:你想独占写,先 `flock(fd, LOCK_EX)`,别人也想知道有没有人在写,他也 `flock`——这样俩人就撞上了,后到的阻塞或拿到 `EAGAIN`。但谁要是不参与这个协议直接写,内核不管。

建议锁是 POSIX flock 的选择——简单、不误伤(不会因为一个进程持锁就让别的进程正常的读写莫名其妙失败),代价是靠大家守规矩。

### 两种模式 + 非阻塞

[file_lock.hpp](../../../kernel/fs/file_lock.hpp) 定义的 operation 位:

```cpp
kLockSh = 1;  // LOCK_SH 共享:多个任务能同时持有(适合「我要读,不希望别人写」)
kLockEx = 2;  // LOCK_EX 排他:独占,互斥所有其他锁(适合「我要写,不希望任何人碰」)
kLockNb = 4;  // LOCK_NB 非阻塞:冲突时返 EAGAIN,而不是阻塞睡觉
kLockUn = 8;  // LOCK_UN 释放
```

冲突规则就两条:

- **共享 vs 共享:不冲突**。多个读者可以同时锁同一个 inode(读书不互相破坏)。
- **涉及排他的都互斥**:排他 vs 任何(共享或排他)都冲突。写会破坏,所以写要独占。

不加 `LOCK_NB` 时,冲突的任务会被挂到 inode 的等待队列上(`wait_enqueue`),等持锁者释放(`wake_all`)再醒;加 `LOCK_NB` 就直接返 `EAGAIN` 走人。

### 锁的 key 是 inode,不是 fd

这是 flock 一个关键设计:**锁挂在 inode 上,不是 fd 上**。两个 fd(甚至两个进程)打开同一个文件,它们锁的是同一个 inode——所以会冲突。这符合直觉:「同一个文件,同一把锁」。

```cpp
// flock 操作的 key 是 inode,owner 是任务
static int64_t flock(Inode* inode, cinux::proc::Task* owner, uint32_t operation);
// close(fd) 时释放该任务在该 inode 上的锁
static void release_task_inode(Inode* inode, cinux::proc::Task* owner);
```

`close(fd)` 触发 `release_task_inode`——关掉的 fd 对应的任务,在该 inode 上的所有锁都释放。这有个简化:`dup` 出来的两个 fd(共享同一个 open-file-description)Linux 下是共享一把锁的,咱们这里简化成「该任务在该 inode 上的锁全释放」,不区分 `dup` 共享。这是个诚实的边界——dup 共享一把锁的精确语义留后续。

### 阻塞怎么实现

持锁冲突时,任务要睡觉等。这复用了咱们 pipe 那套阻塞原语(挂到等待队列 + `prepare_to_wait` + `schedule_blocked`,被 `wake_all` 唤醒):

```cpp
// 冲突且没带 LOCK_NB:挂到这个 inode 的等待队列上,睡觉
net::wait_enqueue(g_waiters, owner);
Scheduler::prepare_to_wait(owner);
g_lock.unlock();                       // 别持锁睡觉(死锁)
Scheduler::schedule_blocked();         // 让出 CPU
g_lock.lock();                         // 醒来重新拿锁、重试
net::wake_all 会由 unlock/release 的路径在释放锁时调,唤醒所有等的人。
```

> 这段「持锁跨阻塞」要小心:睡觉前必须放锁(`g_lock.unlock()`),否则别的任务想释放锁都拿不到锁,睡觉的人永远等不到唤醒——经典死锁。醒来后要重新拿锁再检查一次(可能多个等的人同时醒,只有一个能拿到),这跟 pipe 的阻塞读是同一个模式。

## 把两件事放一起看
