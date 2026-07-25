---
title: 02 · 数据存哪:真在堆上的字节
---

# 数据存哪:真在堆上的字节

## 数据存哪:不是生成的,是真在堆上的字节

先回顾一下家族里前两位。DevFS 的 `/dev/null` 读写不是搬运数据,是**触发动作**:写进去全丢,读出来是 EOF;没有「文件内容」这个概念,只是「写这个 inode 该触发哪个设备行为」。ProcFS 的 `/proc/<pid>/stat` 是**现场生成的伪文件**:读的时候从进程表取数、拼一段 `pid (name) state ppid ...` 文本返回,读完就丢,磁盘和堆上都不存。这俩的共同点是——**没有持久内容**。

tmpfs 不一样。它的文件内容是用户态 `sys_write` 真写进去的字节,得有地方住。所以 `TmpNode` 比 DevFS 的 `DevNode` 多出三件套:

```cpp
struct TmpNode {
    Inode    inode;             // 内嵌;ops + fs_private 由 TmpFs 盖章
    TmpFs*   fs;                // 归属 FS(lock + ino 分配器)
    TmpNode* parent;            // 父目录(根为 nullptr)
    TmpNode* next_sibling;      // 父的 first_child 链上的下一条
    TmpNode* first_child;       // (目录)子链表头
    char     name[kTmpfsNameMax];   // NUL 结尾的项名
    uint8_t* data;              // (文件)堆字节缓冲
    uint64_t capacity;          // (文件)已分配的字节数
    uint64_t size;              // (文件)逻辑内容长度
};
```

(`tmpfs.cpp:19-29`。)前半段(`inode/fs/parent/next_sibling/first_child/name`)是「目录结构」——`fs_private` 指回 `this`,跟 DevFS 的「`fs_private = this`」是**同一个模子**,只不过从「单例 FS 指自己」升级到「per-node 指自己」(每个 node 都能经 `static_cast<TmpNode*>(inode->fs_private)` 找回自身)。后半段 `data/capacity/size` 是「文件内容」——DevFS 没有这部分,因为它压根不存数据。

写路径(`TmpFileOps::write`)是这章最值得拆开看的一段。它做三件事:

```cpp
uint64_t need = offset + count;
if (need > node->capacity) {
    // 扩容:开一个 4KB 对齐的新缓冲,拷前缀、零填 gap、释放旧缓冲
    uint64_t newcap = round_up_capacity(need);
    uint8_t* nd     = new uint8_t[newcap];
    if (node->size > 0) memcpy(nd, node->data, node->size);   // 旧前缀
    if (offset > node->size)
        memset(nd + node->size, 0, offset - node->size);      // gap 零填
    delete[] node->data;
    node->data     = nd;
    node->capacity = newcap;
}
memcpy(node->data + offset, buf, count);   // 真写入
```

(`tmpfs.cpp:126-157`,有删节。)三件事:

**第一,按 4KB 对齐摊销扩容**。`round_up_capacity`(`tmpfs.cpp:35-41`)把 `need` 向上取整到 `kTmpfsGrowthAlign = 4096`(`tmpfs.hpp:58`)的整倍数。为什么不是每字节 realloc?因为 GCC 写中间 `*.o` 的模式是「一坨小 `write()` 调用灌进同一个文件」,如果每次写都按需分配,一兆字节的文件要 realloc 一兆次。4KB 一档,跟 MM 层的页粒度对齐——一个 1MB 文件按 1 字节写、4096 对齐扩容,只需 realloc 256 次(等于 1MB/4KB,每页一次、常数摊销;不是几何倍增那种对数摊销,但相对每字节 realloc 已经是 4096 倍的省事)。

**第二,gap 零填**。当 `offset > node->size`(比如先写 50 字节、再在 offset 4090 续写),新旧 EOF 之间空了一段。这段**必须 `memset` 零填**,不能是新 buffer 里残留的堆字节。`new uint8_t[newcap]` 不保证零初始化(C++ 的 value-init 对 POD 数组是零,但代码显式 `memset` 一下是双保险——万一以后有人把 `new uint8_t[newcap]` 改成 `new uint8_t[newcap]()` 之外的什么,这条零填还在)。test 里专门有一例 `test_grow_past_4k_boundary_and_gap`(`test_tmpfs.cpp:138`)实证:写 50 字节、再在 4090 写 50 字节,读 [50,100) 那段 gap,断言全是零,不是上个用过的堆字节(stale 而不是干净页)。

**第三,`is_page_cacheable()` 的「故意不做」**。这是这章最反直觉的一点。tmpfs 是**内存型** FS,它的内容永远不该进磁盘 PageCache(那个 cache 是给 ext2 这种 file-backed 后端准备的)。可 tmpfs 没有任何一行覆写 `is_page_cacheable()`——它靠 `InodeOps` 基类的**默认实现返回 false** 逃生:

```cpp
bool InodeOps::is_page_cacheable() const {
    return false;
}
```

(`inode.cpp:99-101`。源码里这行是裸 `return false;`,上面的中文注释为本章所加,方便对照「基类默认就 return false」这个事实。)于是 `sys_read` 的路由 gate(`sys_read.cpp:48`)判否,直连 `ops->read`(`sys_read.cpp:64`);`sys_write` 同理(`sys_write.cpp:53` 判否,`sys_write.cpp:66` 直连)。内容永远不进 `g_page_cache`。看起来像「什么都没做」,其实是精心选择的「不做」——默认 false 既能挡住 tmpfs(内存型,不该进磁盘 cache),也是 pipe/pty 这些 transient shim 的同款逃生路径。**这里的正确性不是来自显式代码,是来自一个被故意留在默认值的虚函数**。这是特性,不是漏。
