---
title: 01 · 导引:为什么需要 tmpfs
---

# 导引:为什么需要 tmpfs

> 做完之后,`/tmp` 真能用:boot 自动挂上、busybox 能在里面建目录写文件、`mount -t tmpfs none /mnt/tmp` 能在运行时再挂一个、`umount` 能把整棵树干净回收。一条诚实的边界先说在前头:这是内存型 FS 的第一刀,只做**最小可用**的 tmpfs——无 inode 级 last-close 语义(`unlink` 立即删 node)、非空目录删除返 EIO 而非 ENOTEMPTY、单 per-FS Spinlock 粗粒度、无 size 上限/无 swap 支撑、卸载即丢、`symlink`/`link`/`rename` 未实现。这些不假装做了,留到正文和「这章没做的」里一条条交代。

## 这章咱们要点亮什么

1. **同一个范式的第三次复用**:`FileSystem` 子类 + `InodeOps` 子类 + boot `_init.cpp` 这个 DevFS/ProcFS 立起来的模子,tmpfs 照搬。差别只在「内容是不是真数据」。
2. **内容存哪:不是生成的,是真在堆上的字节**。`TmpNode` 内嵌 `Inode` + `fs_private` 指回自身(从 DevFS 的「单例 FS 指自己」升级到「per-node 可变内容指自己」),文件内容住在 `data/capacity/size` 三件套里。
3. **写路径三件事**:越 capacity 按 4KB 对齐摊销扩容、gap 零填防 stale 堆字节泄漏、靠 `is_page_cacheable()` 默认 false 这条「故意不做」的逃生路径绕开磁盘 PageCache。
4. **目录树怎么长**:单向兄弟链表代替 DevFS 的定长表,支持运行时 `create`/`mkdir`/`unlink` 改树;`lookup` 是真正的多段 walk(扁平的 DevFS/ProcFS 不需要)。
5. **两条挂载通路**:boot 用静态 `g_tmpfs`、unowned(`umount2` 只摘槽);运行时 `sys_mount` 用堆对象、owned=true(`umount2` 走 `free_tree` 递归回收整棵树)。差别全在挂载表那个 `owned` bool。
