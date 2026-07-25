---
title: 04 · 收尾:两件事放一起看 + 范围与边界
---

# 收尾:两件事放一起看 + 范围与边界

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
