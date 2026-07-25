---
title: 02 · 主线一:目录项缓存 dentry cache
---

# 主线一:目录项缓存 dentry cache

### 为什么要缓存

`open("/etc/passwd")` 在咱们没缓存时要做这些事:从根 inode 出发,`lookup_child(根, "etc")` → ext2 去读根目录的数据块、扫目录项、找到 `etc` 的 inode 号、装进 inode 缓存、返回。然后 `lookup_child(etc_inode, "passwd")` 又来一遍。每一次 `lookup_child` 对 ext2 来说都可能是一次磁盘读。

可 `/etc/passwd` 这个路径,在一次启动里第一次解析完,**`(根, "etc") → etc_inode` 这个映射就不变了**(除非有人 `rmdir etc` 或 `rename`)。第二次 `open("/etc/passwd")` 完全没必要再去问 ext2——咱们第一次已经知道了答案。

dentry cache 就是存这个映射的:`(parent Inode*, name) → child Inode*`。

### 为什么 key 是 `(父 inode, 名字)`,不是路径字符串

一个直观想法是按整条路径(`/etc/passwd`)做 key。但路径字符串做 key 有两个麻烦:一是每段都要拼字符串、比较字符串,慢;二是符号链接、`.`/`..`、挂载点会让「同一条逻辑路径」对应不同的字符串写法。

咱们用 **(父 inode 指针, 下一级名字)** 做 key。这依赖一个不变式:**同一个逻辑目录,多次解析拿到的是同一个 `Inode*`**。ext2 的 inode 缓存保证了这一点(按 inode 号缓存,同一个号同一个对象);procfs/devfs 的固定池子也保证。所以 `(父 inode*, 名字)` 在一次启动里是稳定 key,不会因为同义路径(比如有符号链接)而漏掉。

### 三个操作 + 一个不变式

[dentry.hpp](../../../kernel/fs/dentry.hpp) 的全部接口就三个:

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
