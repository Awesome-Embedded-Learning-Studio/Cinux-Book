---
title: Lab 078 · VFS 地基验证:组件遍历、引用计数、inode 缓存
---

# Lab 078 · VFS 地基验证:组件遍历、引用计数、inode 缓存

> 对应 `document/book/08-filesystem/078-vfs-groundwork-lookup-refcount.md`。验证档 **B 档**:地基是内部重写、没有单一用户可见新能力,lab 靠跑机制测 + 看指标验证「三块地基都按设计答」。三组测:`test_syscall_ext2`(符号链接 follow + 环检测)、`test_sys_pipe`(dup last-close EOF)、ext2 inode 缓存(别名场景)。

## 目标

确认四件事:

1. **`vfs_lookup` 组件遍历 + 符号链接 follow**:绝对/相对符号链接都能 follow 到目标;`a→b→a` 的环被判 `ELOOP`(跟 > 40);
2. **inode 引用计数 + last-close**:`dup` 出来的多个 fd,关到**最后一个**才触发 release(pipe 才发 EOF),中间关不发;
3. **ext2 inode 缓存不别名**:同一个 ino 多次解析拿到**同一个 `Inode*`**(堆对象地址稳定);缓存满时只驱逐 `refcount==0` 的;
4. **inode_unref 在锁外**:close 路径 detach 在 fd 表锁内、release 在锁外(看代码路径,不持锁调可能 block 的 release)。

## 步骤

### 1. 编 + 跑三组机制测

```bash
cmake --build build -j$(nproc) 2>&1 | grep -iE 'vfs_lookup|file\.cpp|ext2_inode|Built target big_kernel_test' | head
cmake --build build --target run-kernel-test 2>&1 | grep -aE 'symlink|follow|loop|pipe.*dup|last.close|Tests:' | head -20
```

应看到 `vfs_lookup.cpp`/`file.cpp`/`ext2_inode.cpp` 都编了,机制测里符号链接和 pipe dup 用例 `[PASS]`,末尾 `Tests: 1084 passed, 0 failed`。

### 2. 验符号链接 follow + 环检测

`test_syscall_ext2` 里跟 `vfs_lookup` 一起进来的三条用例覆盖:

- **绝对符号链接 follow**:`/link → /target`,lookup `/link`(带 Follow)解析到 `/target` 的 inode;
- **相对符号链接 follow**:`/dir/link → ../sibling`,相对目标拼到父目录 `/dir/..` = `/`,解析到 `/sibling`;
- **环检测**:构造 `a→b→a`(或自指 `a→a`),lookup 返 `ELOOP`(`-40`),不挂死。

要在串口确认,临时在 [vfs_lookup.cpp](kernel/fs/vfs_lookup.cpp) 的 `++depth > 40` 分支加 `kprintf("ELOOP depth=%u\n", depth)` 重跑,能看到环路径触发了深度封顶。验完删打印。

### 3. 验 dup last-close EOF(引用计数的核心收益)

`test_sys_pipe` 的 `test_sys_pipe_dup_last_close_eof`:dup 一个 pipe 读端 fd 成两个,关掉一个 → **不发 EOF**(对端还能写、读端还能读);再关掉第二个 → 才发 EOF。

这验的是主线二的「last-close release」:`dup` 让 `Inode::refcount` = 2,第一个 close 减到 1(没到 0,不 release),第二个 close 减到 0(才 release → pipe 内层 read_refs 到 0 → EOF)。旧代码(没引用计数)第一个 close 就发 EOF,这条测会挂。

### 4. 验 ext2 inode 缓存:同 ino 同指针 + 不驱逐活引用

ext2 的别名场景(章节主线三)靠 `test_ext2_*` 系列 + 缓存内部断言:

- **同 ino 同指针**:连续两次 `get_cached_inode(ino)` 拿到的 `Inode*` 相等(同一个堆对象,不是两次 new);
- **驱逐只挑 refcount==0**:这层在单线程测里不容易触发「缓存满 + 全活」,但能靠 `grep` 确认 `get_cached_inode` 的驱逐循环判的是 `vfs_inode.refcount == 0`:

```bash
grep -nA2 'victim_prev\|refcount == 0' kernel/fs/ext2/ext2_inode.cpp | head
```

应看到驱逐扫描挑的是 `(*pp)->vfs_inode.refcount == 0` 的条目,全活(`victim_prev == nullptr`)时返 nullptr + 打印「cache at cap ... cannot cache」,绝不覆盖活对象。

### 5.(代码路径检查)inode_unref 在锁外

主线二要求 `release`(可能 block)在 fd 表锁外调。确认 [file.cpp](kernel/fs/file.cpp) 的 `FDTable::close`:

```bash
grep -nA15 'int FDTable::close' kernel/fs/file.cpp | grep -E 'guard|detach|delete file|inode_unref|}'
```

应看到模式:进 `{ auto g = lock_.guard(); ... detach(file/inode 出来) }`(锁内只 detach)→ 锁外 `delete file; inode_unref(inode);`(release 在 unref 里、锁外跑)。如果 `inode_unref` 在锁内,block 会卡整个 fd 表——这是回归点。

## 范围与边界

- 地基是**单核正确**的;SMP 下的并发竞态(`get_cached_inode` 的 walk/evict/insert 要加 `inode_cache_lock_`、引用计数跨核原子)在隔壁并发修复弧加,本 lab 不验 SMP 压力。
- `vfs_lookup` 的环检测封顶 40 是 Linux `MAXSYMLINKS`;合法用例不会撞,撞了就是真环或恶意构造。
- ext2 inode 缓存软 cap 4096,正常工作集远小于它;「缓存满 + 全活」是容量问题(返失败),不是正确性问题。
