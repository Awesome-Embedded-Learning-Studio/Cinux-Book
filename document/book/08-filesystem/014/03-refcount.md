---
title: 03 · 主线二:inode 引用计数与 last-close release
---

# 主线二:inode 引用计数与 last-close release

### 以前为什么凑合能跑、什么时候崩

以前 `Inode` 结构没有「当前几个 fd 在用我」这个计数。`FDTable::close(fd)` 直接 `delete` 掉 File 对象、把 fd 槽清空,inode 的生死完全由底层文件系统自己的缓存管(ext2 的 inode 缓存自己决定何时驱逐)。pipe 的 EOF、socket 的 FIN 这种「关 fd 时要通知对端」的逻辑,没有统一的钩子。

这套在「单进程、打开-关闭一一对应」时没事。可一旦 `dup`(同一个 inode 被两个 fd 指着)或 `fork`(子进程继承一份 fd 表):

- pipe 的读端被 `dup` 成两个 fd。关掉一个,旧代码可能就发 EOF 了——可另一个 fd 还在读呢,EOF 是错的。这就是 `ls | grep` 管道偶发崩的根因。
- 任何「缓存返回 inode 指针」的设计(下一章的 dentry cache、ext2 的 inode 缓存),都面临「我把指针给出去了,底层会不会在我用着的时候把它回收掉」——没有引用计数,这个问题没法回答。

### 外层 inode 引用计数 + last-close release

修法是给 `Inode` 加一个**外层引用计数** `refcount`,语义是「当前有几个 File(fd)指着这个 inode」:

```cpp
struct Inode {
    // ...
    uint32_t refcount{0};   // DEBT-023:几个 fd 指着我
};

// file.cpp:进出 FDTable 时 bump/dec
void inode_ref(Inode* i)   { if (i) __atomic_add_fetch(&i->refcount, 1, ...); }
void inode_unref(Inode* i) {
    if (i && i->ops && __atomic_sub_fetch(&i->refcount, 1, ...) == 0) {
        i->ops->release(i);   // 最后一个引用关了 → 通知底层(pipe EOF / socket FIN)
    }
}
```

`alloc`/`dup`/`dup2`/`set` 时 `inode_ref`(新 fd 拿一个引用),`close` 时 `inode_unref`(fd 释放它的引用)。**`release` 只在 refcount 减到 0(最后一个 fd 关了)时才调**——这就是「last-close 语义」,治了 `dup` 误发 EOF 的病。

`InodeOps::release` 是个虚函数,默认 no-op(普通文件、ext2 inode 不需要关 fd 时干啥);pipe 的读/写端 override 它——`release` 时 `release_read_ref`/`release_write_ref`,pipe 内层再判断「这个端的所有 fd 是不是都关了」来发 EOF/BrokenPipe。

> **两层计数**:注意 pipe 这里有**两层**——外层 `Inode::refcount`(这个 inode 几个 fd 在用)+ 内层 `Pipe::read_refs_`/`write_refs_`(这个端几个 inode 在指)。外层到 0 触发 `release`,`release` drop 一个内层引用;内层到 0 才真发 EOF。dup 一个 pipe 读端 fd:外层 refcount 2,内层 read_refs 2。关一个 fd:外层 2→1(没到 0,不 release),pipe 不发 EOF——对。两个 fd 都关:外层 1→0(两次,第一次到 1 第二次到 0)→ 第二次 release → 内层 read_refs 2→1→0 → 发 EOF。

### 为什么 refcount 要原子、release 要锁外

`refcount` 用 `__atomic_*_acq_rel`:fork 之后父子进程的 fd 表在不同 CPU 上可能同时 `inode_unref` 同一个 inode(共享的 pipe 端),非原子就丢更新。`acq_rel` 保证「减到 0 的那个 CPU 看到之前所有对 inode 的写」。

`release` 在 `FDTable::close` 里**锁外**调:close 先在 fd 表锁内把 File detach 出来(`fd→null`),放锁,再 `delete file` + `inode_unref`。为什么?因为 `release` 可能 block(pipe close_writer 要唤醒阻塞的对端、socket 要发 FIN),如果持着 fd 表锁调,整个进程的 fd 表操作都被卡住。锁内只做 detach 这个 O(1) 动作,重活放锁外。
