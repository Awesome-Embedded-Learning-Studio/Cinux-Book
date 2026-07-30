---
title: 03 · 主线一:目录项缓存 dentry cache
---

# 主线一:目录项缓存 dentry cache

上一篇把"为什么要缓存路径解析、key 怎么选、pin 和失效是什么"都讲透了,这里看 Cinux 的 `DentryCache` 怎么落地。

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

上一篇讲的 pin 不变式,落地就一行:`add` 给 child 做一次 `inode_ref`(缓存自己持有一个引用),`invalidate` 配对 `inode_unref`。于是只要某段路径在缓存里,它指向的 inode 就不会被释放——`vfs_lookup` 命中时返回的那个指针一定是活的。

> 为什么命中要返回**新引用**、而不是直接返回缓存里的指针?因为调用方拿到 child 后可能立刻走开(比如 follow 符号链接、或返回给 syscall),而缓存在这期间可能因为 `invalidate` 把自己的那条引用 unref 掉了。如果直接返回缓存内部指针,调用方手里的指针可能瞬间悬垂。返回新引用 = 调用方有自己那条命,和缓存解耦。这就是上一篇「钉住」要付的代价——一次缓存命中,一次 `inode_ref` + 一次配对的 `inode_unref`。

### 失效:`unlink`/`rmdir`/`rename` 之后

上一篇讲过,名字一旦没了或指向变了,缓存条目就过期。落地是这三个 syscall 在成功后都调 `invalidate`:

```cpp
// sys_unlink 成功删掉 (dir, name) 后
DentryCache::invalidate(dir, name, namelen);
```

不调会怎样?`unlink /tmp/foo` 之后,缓存还记着 `(tmp_inode, "foo") → foo_inode`,下次 `open("/tmp/foo")` 命中缓存、拿到 foo_inode——可这个文件其实已经删了,拿到的是个悬空对象(虽然引用计数让它还活着,但它已经不在目录里了)。所以失效不是可选项,是缓存正确性的另一半。

> 边界说清楚:**这个缓存没有淘汰策略(LRU shrink)**。条目只增不减(除了 `invalidate`),启动越久积越多。这对一个玩具 OS 够用(一次启动解析的目录有限);要做成有界的,得加 LRU + 一个上限,那是后续工程。

### 对照 Linux:不抽 dentry 对象,也少了两件进阶

Cinux 这套「(父 inode, 名字) → 子 inode」的直接映射,在 Linux 里有个更专门、也更重的载体——独立的 **dentry 对象**(`struct dentry`)。Linux 给每个目录项立这么一个对象,字段里挂 inode 指针、父 dentry、哈希链、LRU 链,dentry 们还靠父子指针组成一棵跟文件空间同形的目录树。Cinux 没抽这个中间对象:`DentryCache` 直接拿 `(父 Inode*, 名字)` 当 key、`Inode*` 当 value,省掉了独立 dentry 这一整层(连带省掉 dentry 树、dentry 自己的引用计数),代价是拿一个条目要多解一次引用。

但上一篇讲的 pin 不变式,两边想到一起去了——Linux 那条规矩叫「dcache 是 icache 的主人」:只要一个 dentry 还在,它指向的 inode 就被钉住、不能回收。Cinux 的 `add`/`invalidate` 配对 `inode_ref`/`inode_unref`,是同一个思路。

至于上一篇点到的淘汰和 negative 两条,Cinux 这版都没做:没有 LRU shrink(条目只增不减,只在 `invalidate` 时减),也不缓存 negative(查不存在的名字不进表,lookup miss 直接返 nullptr)。这两件 Linux 的 dcache 都有,是 production 文件系统的标配;Cinux 留作后续。
