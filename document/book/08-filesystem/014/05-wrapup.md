---
title: 05 · 收尾:三块地基拧成一根绳 + 范围与边界
---

# 收尾:三块地基拧成一根绳 + 范围与边界

把这一章的三块放一起看,它们其实是在回答同一个问题的三个层面:**「拿到一个 inode 指针之后,我能用它多久、用它的时候它会不会变」**。

- `vfs_lookup`(主线一)回答**怎么拿到**对的那个 inode(逐层遍历、follow 链接、不会拿错)。
- 引用计数(主线二)回答**拿到之后它多久有效**(只要我持引用,它就不被释放;我放了,最后一个放的人负责通知)。
- ext2 inode 缓存(主线三)回答**底层缓存在这期间会不会捣乱**(堆 + refcount,被引用的对象绝不被动)。

下一章的 dentry cache(缓存解析过的路径)和 flock(文件锁)就直接建在这三块上:dentry 命中返回新引用(主线二)、`invalidate` 配对 unref、缓存项 pin 住 child(主线三的同款思路)。地基打牢了,上面那两个功能就是顺水推舟。

## 范围与边界(诚实说)

- **`vfs_lookup` 不跨进程 chroot**:咱们没有 chroot,路径解析都以全局根为起点;有 chroot 的话每进程根要进 `vfs_lookup` 的入参。
- **引用计数是「外层 fd 计数」,不是「inode 本身的生死」**:inode 的最终释放仍由底层文件系统(ext2 的 inode 缓存)决定;引用计数只管「几个 fd 指着 + last-close release」。一个 refcount==0 的 ext2 inode 仍可能在缓存里(没被驱逐),只是没有 fd 指着它。
- **ext2 inode 缓存的软 cap 是 4096**:`EXT2_INODE_CACHE_MAX`。超过且全活(refcount>0)就缓存失败(返 nullptr),不腐蚀。正常工作集远小于 4096;真超了是容量问题,不是正确性问题。
- **SMP 下的 inode 缓存遍历加锁**:`get_cached_inode` 整个 walk/evict/insert 套 `inode_cache_lock_`(持锁跨读盘 I/O),这是另一弧(并发竞态修复)加的,这章的 cache 重设计本身是单核正确的,加锁是后续。

> 这一章三块地基都有随它们一起进来的机制测(`test_syscall_ext2` 的符号链接 follow/环、`test_sys_pipe` 的 dup last-close EOF、ext2 inode 缓存的别名场景),`run-kernel-test` 跑过、绿。它们是 VFS 这层「能正经用」的底线——后面所有文件系统相关的功能(dentry cache、flock、busybox 频繁 open/execve、GCC 自举跑真编译)都站在它们上面。
