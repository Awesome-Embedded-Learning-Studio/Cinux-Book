---
title: 079 · VFS 收尾:路径解析缓存与文件锁
---

# 079 · VFS 收尾:路径解析缓存与文件锁

> 咱们的 VFS 已经能解析路径、能读写文件、能挂载不同的文件系统。可离「正经能用」还差两件:第一,**每次 `open` 都要把路径从头到尾走一遍**——`/etc/passwd` 每次都重新解析 `/`、`etc`、`passwd` 三层,每一层都去问底层文件系统(ext2 还要查盘上的目录块),明明上一秒刚解析过;第二,**两个进程能同时写同一个文件把彼此覆盖**,内核不管——`cp a b` 和 `cp c b` 撞上就是谁后写谁赢,数据丢了内核一声不吭。
>
> 这一章给 VFS 补最后两块:**目录项缓存(dentry cache)**记住解析过的路径,**文件锁(flock)**让进程能协商谁先写。两件事各自不大,但加上它们之后,「解析一次、反复用」+「写之前先锁」就成了咱们 VFS 的默认行为。
>
> A 档:punchline 是机制测——`run-kernel-test` 里 `run_dentry_tests` 验证缓存命中/未命中/失效,`run_flock_tests` 验证共享锁能并发、排他锁互斥、带 `LOCK_NB` 冲突返 `EAGAIN`。串口看到 `[PASS]` 即两条机制都按 Linux 语义答了。

## 这章咱们要点亮什么

1. **路径解析是可以缓存的**:同一段路径(`(父 inode, 名字) → 子 inode`)在一次启动里结果不变,所以可以把第一次解析的结果存起来,下次直接拿。难点不在「存」,在「什么时候这个映射会变」——`unlink`/`rmdir`/`rename` 会让它失效。
2. **缓存项得「钉住」它指向的 inode**:被缓存的子 inode 不能在缓存还指着它的时候被释放(否则缓存就成了悬垂指针)。这靠咱们已经就位的 inode 引用计数——缓存命中时返回一个**新引用**,调用方用完 `inode_unref`。
3. **flock(2) 是「建议锁」**:内核只记「谁锁了、锁的什么模式」,不强制阻止读写——靠进程自觉先锁再操作。`LOCK_SH`(共享,多个读者能同时持有)vs `LOCK_EX`(排他,独占);`LOCK_NB` 让冲突时返 `EAGAIN` 而不是阻塞。
4. **锁的 key 是 inode 不是 fd**:两个 fd 打开同一个文件,它们锁的是同一个东西(会冲突);`close(fd)` 释放该任务在该 inode 上的锁——这是「简化版」语义(Linux 按 open-file-description 区分 `dup` 共享,咱们这层 defer)。

## 这章建在什么之上

dentry cache 和 flock 都建在前面已经就位的两块地基上,这里只点一下、不重讲:

- **`vfs_lookup` 组件遍历**:路径解析已经会一层层 `lookup_child` 走下去、会 follow 符号链接。dentry cache 只是给这条走路径的过程加一层「先查缓存」。
- **inode 引用计数**(`inode_ref`/`inode_unref` + `InodeOps::release`):缓存命中要返回一个新引用、`unlink` 要让缓存失效后旧引用还能安全用完——这套计数是安全缓存的底。

两块地基都在前几个 commit 里落地了(随这次 VFS 收尾一起进来的),咱们这章直接消费它们。

## 主线一:目录项缓存——记住解析过的路径

### 为什么要缓存

`open("/etc/passwd")` 在咱们没缓存时要做这些事:从根 inode 出发,`lookup_child(根, "etc")` → ext2 去读根目录的数据块、扫目录项、找到 `etc` 的 inode 号、装进 inode 缓存、返回。然后 `lookup_child(etc_inode, "passwd")` 又来一遍。每一次 `lookup_child` 对 ext2 来说都可能是一次磁盘读。

可 `/etc/passwd` 这个路径,在一次启动里第一次解析完,**`(根, "etc") → etc_inode` 这个映射就不变了**(除非有人 `rmdir etc` 或 `rename`)。第二次 `open("/etc/passwd")` 完全没必要再去问 ext2——咱们第一次已经知道了答案。

dentry cache 就是存这个映射的:`(parent Inode*, name) → child Inode*`。

### 为什么 key 是 `(父 inode, 名字)`,不是路径字符串

一个直观想法是按整条路径(`/etc/passwd`)做 key。但路径字符串做 key 有两个麻烦:一是每段都要拼字符串、比较字符串,慢;二是符号链接、`.`/`..`、挂载点会让「同一条逻辑路径」对应不同的字符串写法。

咱们用 **(父 inode 指针, 下一级名字)** 做 key。这依赖一个不变式:**同一个逻辑目录,多次解析拿到的是同一个 `Inode*`**。ext2 的 inode 缓存保证了这一点(按 inode 号缓存,同一个号同一个对象);procfs/devfs 的固定池子也保证。所以 `(父 inode*, 名字)` 在一次启动里是稳定 key,不会因为同义路径(比如有符号链接)而漏掉。

### 三个操作 + 一个不变式

[dentry.hpp](kernel/fs/dentry.hpp) 的全部接口就三个:

```cpp
// 命中 -> 返回一个新引用过的 child inode(调用方负责 unref);未命中 -> nullptr
static Inode* lookup(const Inode* parent, const char* name, uint32_t namelen);
// 缓存 (parent, name) -> child。pin 住 child(给它加一个引用)
static void add(const Inode* parent, const char* name, uint32_t namelen, Inode* child);
// 让 (parent, name) 失效(unpin 它的 child)。unlink/rmdir/rename 成功后调
static void invalidate(const Inode* parent, const char* name, uint32_t namelen);
```

`vfs_lookup` 的组件遍历里,每一层先问缓存:

```cpp
Inode* child = DentryCache::lookup(cur, p, comp_len);   // 先查缓存
if (child == nullptr) {                                  // 未命中才问底层
    auto child_r = fs->lookup_child(cur, p, comp_len);
    if (!child_r.ok()) { /* ... */ }
    child = child_r.value();
    DentryCache::add(cur, p, comp_len, child);           // 填回缓存
}
```

**关键不变式:缓存项 pin 住它的 child**。`add` 给 child 做一次 `inode_ref`(缓存自己持有一个引用),`invalidate` 配对 `inode_unref`。这样只要某段路径在缓存里,它指向的 inode 就不会被释放——`vfs_lookup` 命中时返回的那个指针,一定是活的。

> 为什么命中要返回**新引用**、而不是直接返回缓存里的指针?因为调用方拿到 child 后可能立刻走开(比如 follow 符号链接、或返回给 syscall),而缓存在这期间可能因为 `invalidate` 把自己的那条引用 unref 掉了。如果直接返回缓存内部指针,调用方手里的指针可能瞬间悬垂。返回新引用 = 调用方有自己那条命,和缓存解耦。这就是上一节「钉住」要付的代价——一次缓存命中,一次 `inode_ref` + 一次配对的 `inode_unref`。

### 失效:`unlink`/`rmdir`/`rename` 之后

缓存记住的是「这条名字解析到这个 inode」。一旦这个名字没了(`unlink` 删掉、`rename` 改名),或者指向变了(`rename` 把别的东西搬到这个名字下),缓存条目就过期了。所以这三个 syscall 在成功后都得调 `invalidate`:

```cpp
// sys_unlink 成功删掉 (dir, name) 后
DentryCache::invalidate(dir, name, namelen);
```

不调会怎样?`unlink /tmp/foo` 之后,缓存还记着 `(tmp_inode, "foo") → foo_inode`,下次 `open("/tmp/foo")` 命中缓存、拿到 foo_inode——可这个文件其实已经删了,拿到的是个悬空对象(虽然引用计数让它还活着,但它已经不在目录里了)。所以失效不是可选项,是缓存正确性的另一半。

> 边界说清楚:**这个缓存没有淘汰策略(LRU shrink)**。条目只增不减(除了 `invalidate`),启动越久积越多。这对一个玩具 OS 够用(一次启动解析的目录有限);要做成有界的,得加 LRU + 一个上限,那是后续工程。

## 主线二:文件锁 flock——让进程协商谁先写

### 建议锁是什么意思

flock(2) 给的是**建议锁(advisory lock)**:内核只记录「这个 inode 被这个任务以什么模式锁了」,**不强制**阻止别人读写。一个进程不调 `flock` 直接写,内核照样让它写——锁只对**也调了 flock 的进程**有意义。

这跟「强制锁」(mandatory lock,读写操作本身会被内核拦)不一样。Linux 的 flock 是建议锁,进程之间靠自觉:你想独占写,先 `flock(fd, LOCK_EX)`,别人也想知道有没有人在写,他也 `flock`——这样俩人就撞上了,后到的阻塞或拿到 `EAGAIN`。但谁要是不参与这个协议直接写,内核不管。

建议锁是 POSIX flock 的选择——简单、不误伤(不会因为一个进程持锁就让别的进程正常的读写莫名其妙失败),代价是靠大家守规矩。

### 两种模式 + 非阻塞

[file_lock.hpp](kernel/fs/file_lock.hpp) 定义的 operation 位:

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

dentry cache 和 flock 解决的是 VFS 的两个不同维度:

- **dentry cache 解决「快」**:同样的路径别解析第二遍。它是**内核内部**的优化,对用户态透明(用户调 `open` 不知道有没有缓存)。
- **flock 解决「对」**:让并发进程能协商对同一文件的访问。它是**用户态可见**的 API(进程主动调 `flock`)。

两个都建在 inode 引用计数上:缓存命中返回新引用(调方 unref),`close` 释放锁(顺带释放 fd 的 inode 引用)。引用计数是 VFS 这一层「对象生命周期」的通用语言——缓存和锁都靠它保证手里的指针不会突然失效。

至此 VFS 的「finale」就齐了:路径解析有缓存、文件访问有锁、inode 生命周期有计数。再往后的 ext2 收尾(独立库化、间接块竞态修复)是文件系统**实现**层面的加固,跟 VFS 这层抽象关系不大了——那是另一章的事。

## 范围与边界(诚实说)

- **dentry cache 无淘汰**:条目只增不减(除 `invalidate`),没有 LRU 上限。玩具 OS 够用,有界缓存是后续。
- **flock 是建议锁、按 task 粗粒度**:不强制拦读写;`close` 释放该任务在该 inode 上全部锁,不区分 `dup` 共享一个 open-file-description 的 Linux 精确语义。
- **`sys_mount` 的块设备链没进来**:flock 和 dentry 都落地了,但 `mount -t` 接块设备(用 block_registry 注册的盘)这部分依赖另一条还没回迁的弧(GCC 自举弧里的 `sys_mount`),本轮 reduced 掉了——dentry/flock 不依赖它,能独立讲、独立验。

> 这一章的两条机制都有专门的机制测(dentry 的命中/失效、flock 的共享/排他/非阻塞),`run-kernel-test` 里跑过、绿。但它们都是「单核、单线程测试 harness」下验的——真正的并发竞态(dentry 在 SMP 下的增删查、flock 在多核同时 lock 的唤醒)需要 SMP + 动态竞争检测的压力测试才能暴露,那套基建(IPI TLB shootdown、race-detect)在隔壁的正确性弧里,不在这章。
