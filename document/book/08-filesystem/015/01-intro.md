---
title: 01 · 导引:还差缓存与文件锁
---

# 导引:还差缓存与文件锁

> punchline 是机制测——`run-kernel-test` 里 `run_dentry_tests` 验证缓存命中/未命中/失效,`run_flock_tests` 验证共享锁能并发、排他锁互斥、带 `LOCK_NB` 冲突返 `EAGAIN`。串口看到 `[PASS]` 即两条机制都按 Linux 语义答了。

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
