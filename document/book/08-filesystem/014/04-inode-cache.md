---
title: 04 · 主线三:ext2 inode 缓存——堆分配 + 引用计数驱动回收
---

# 主线三:ext2 inode 缓存——堆分配 + 引用计数驱动回收

## 主线三:ext2 inode 缓存——从「驱逐覆盖」到「堆 + 引用计数」

### 别名 UAF:最阴的 bug

ext2 的 inode 缓存以前是个**固定值数组** `Ext2CachedInode inode_cache_[N]`,每个 slot 装一个缓存条目。要缓存一个新 inode 而所有 slot 都满了,就**驱逐**一个(slot 0 是根,不动;其他按哈希挑一个),把那个 slot 覆盖成新 inode。

问题:**被驱逐的那个 slot,可能正被某个活 fd 或 VMA 指着**。比如:

1. `open("/bin/ld")` → ext2 把 ld 的 inode 装进 slot 5,返回 slot 5 的 `Inode*` 给 fd。
2. 程序跑别的,触发了对另一个 inode 的缓存,slot 5 被驱逐、覆盖成新 inode。
3. 程序从 fd 读 ld 的内容 → 走的是 slot 5 的 `Inode*` → 可 slot 5 现在是**另一个文件**了 → 读到错的字节。

这个 bug 最阴在:**没有内存错误**(slot 还在、指针还合法、读出来的是合法的别的文件数据),ASAN 看不见、KASAN 看不见,只有「数据偶尔错」这种说不清的症状。它是 GCC 自举时 `ld`/`cc1` 偶发崩的根因之一。

### 结构性修法:堆分配 + 引用计数驱动的回收

[c6abfff / ext2_inode.cpp](../../../libs/ext2/ext2_inode.cpp) 的 `get_cached_inode` 重写成:**条目是堆分配的对象,对象的地址是它的身份;只要有人引用(refcount > 0),它就绝不被移动、绝不被重填。**

```cpp
struct Ext2CachedInode {
    Ext2Inode        disk_inode;
    Inode            vfs_inode;
    uint32_t         ino{0};
    bool             stale{false};            // 被带外改过(chmod/unlink),命中时原地刷新
    Ext2CachedInode* hash_next{nullptr};      // 分离链 hash(ino % SIZE 桶)
};

Inode* Ext2::get_cached_inode(uint32_t ino) {
    // 1. 查 hash 桶:命中 → 若 stale 原地重读 → inode_ref(给调用方一个引用)→ 返回
    // 2. 未命中 + 软 cap 满:驱逐一个 refcount==0 的(全活就失败,绝不腐蚀)
    // 3. new 一个堆对象,读盘,inode_ref,挂到桶头,返回
}
```

三个关键点:

1. **堆分配 + hash 桶**:对象 `new` 出来,地址固定。用 `ino % SIZE` 的分离链 hash 组织,不再是用固定数组下标。
2. **引用计数驱动回收**:结合上一节的 `Inode::refcount`——`get_cached_inode` 返回前 `inode_ref`(缓存自己持一个引用 + 给调用方一个),调用方 `inode_unref`。软 cap 满时只驱逐 `refcount==0`(没人用的)对象;有活引用的对象绝不驱逐。
3. **`stale` 而不是驱逐**:`chmod`/`chown`/`utimensat`/`unlink` 改了一个**正被引用**的 inode,不能驱逐(有活引用),改成标记 `stale`,下次命中时**原地重读盘**(同一个对象、同一个地址,刷新内容)。只有 refcount==0 的过期条目才直接释放。

这套下来,「被覆盖」从结构上不可能了——活引用的对象地址永不变,调用方手里的指针永远指着自己当初拿的那个 inode。

> **这个修法依赖前两块地基**:引用计数(主线二)让「refcount==0 才能驱逐」可判断;`vfs_lookup` 的组件遍历(主线一)让 `lookup_child` 返回的 inode 能安全穿过缓存层。三块地基是一体的——这也是为什么它们要一起进、不能拆。

## 三块地基拧成一根绳
