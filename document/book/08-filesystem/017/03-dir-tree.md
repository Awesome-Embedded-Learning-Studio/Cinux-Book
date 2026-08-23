---
title: 03 · 目录树:单向兄弟链表代替定长表
---

# 目录树:单向兄弟链表代替定长表

## 目录树怎么长:单向兄弟链表代替定长表

回顾家族前两位的目录结构。DevFS 是定长数组 `DevNode nodes_[DEVFS_MAX_NODES]`(`devfs.hpp:179`,`DEVFS_MAX_NODES = 16` 在 `devfs.hpp:40`),`mount()` 时建好 null/zero/console,**结构焊死**,只能 `readdir`/`stat`。ProcFS 是按 PID 索引的固定池(`pid_dir_inodes_[257]` 等),PID 有上限所以也能定长,**挂载时结构就固定**。这俩都不支持运行时改树——DevFS 加不了新设备节点,ProcFS 加不了新 PID 目录(进程创建靠 PID allocator,不是靠 FS 的 create)。

tmpfs 必须支持运行时 `create`/`mkdir`/`unlink` 改树——用户随时 `touch` 新文件、`mkdir` 新目录、`rm` 删东西。定长表不行了(不知道上限),解法是**单向兄弟链表**:`TmpNode` 用 `first_child` + `next_sibling` 两条指针组成无上限的链(头插,无尾指针),`parent` 指针支持回溯。

建节点是 `create`/`mkdir` 共用的 `make_node`(`tmpfs.cpp:314-346`):

```cpp
ErrorOr<Inode*> make_node(Inode* dir_inode, const char* name, uint32_t namelen,
                          InodeType type) {
    if (namelen + 1 > kTmpfsNameMax) return Error::InvalidArgument;   // 名字太长
    auto* dir = static_cast<TmpNode*>(dir_inode->fs_private);
    auto  g   = dir->fs->lock_.guard();

    if (find_child(dir, name, namelen) != nullptr) return Error::AlreadyExists;  // 去重

    auto* node = new TmpNode{};                  // value-init:指针 nullptr、size 0
    memcpy(node->name, name, namelen);
    node->name[namelen] = '\0';
    node->fs            = dir->fs;
    node->parent        = dir;
    node->inode.ino     = dir->fs->alloc_ino();  // 单调递增的 inode 号
    node->inode.type    = type;
    node->inode.ops = (type == InodeType::Directory) ? dir->fs->dir_ops()
                                                     : dir->fs->file_ops();
    node->inode.fs_private = node;               // fs_private 指回自身
    node->inode.mode = (type == InodeType::Directory) ? (kTmpfsSIfDir | 0755)
                                                      : (kTmpfsSIfReg | 0644);

    node->next_sibling = dir->first_child;       // 头插
    dir->first_child   = node;
    return &node->inode;
}
```

(`tmpfs.cpp:314-346`,有删节——省略了 `nlink` 初始化等非主线的样板行。)几个细节:**ops 是按类型从 FS 单例取的**——同类 inode 共享一个 ops 实例(所有目录共享 `dir_ops()`、所有文件共享 `file_ops()`),同 DevFS 共享一个 `DevDirOps`。这不是每节点一个虚表,是每类一个。`fs_private` 指回自身,让 ops 经 `static_cast` 找回 `TmpNode`。

`unlink`(`tmpfs.cpp:256-288`)带 `prev` 指针摘链(单向链表删中间节点必须记前驱):

```cpp
TmpNode* prev = nullptr;
TmpNode* c    = dir->first_child;
while (c != nullptr && !name_matches(c, name, namelen)) {
    prev = c;
    c    = c->next_sibling;
}
if (c == nullptr) return Error::NotFound;
// 非空目录拒删
if (c->inode.type == InodeType::Directory && c->first_child != nullptr) {
    return Error::IOError;
}
if (prev == nullptr) dir->first_child = c->next_sibling;
else                 prev->next_sibling = c->next_sibling;
delete[] c->data;   // delete[] nullptr 是安全 no-op
delete c;
```

(`tmpfs.cpp:256-288`,有删节。)

> **非空目录删除返 EIO,不是 ENOTEMPTY**。Cinux-Base(彼时子模块,现已并回 `libs/base`)的 `Error` 枚举没有 `DirectoryNotEmpty` 这一项,所以 `unlink` 一个非空目录返 `Error::IOError`(`tmpfs.cpp:277-279`),syscall 边界映射成 `kEio`。代码注释明说这是已知缺口。**契约层满足**(操作失败,调用方拿到的就是「这事儿干不成」),但 errno 值跟 Linux 不一致——Linux 是 `ENOTEMPTY`。这带来一个测试纪律:测试和教程只能断言「op 失败」(返回非零),**不能断言具体 errno 等于 `ENOTEMPTY`**。这是 ABI/契约分层的一个好教学点:契约库的枚举缺项,会让一个语义清晰的失败,落成一个语义模糊的 errno。留给 Cinux-Base 补枚举项后修(并回后可就地加)。

`lookup`(`tmpfs.cpp:405-453`)是 tmpfs 比 DevFS/ProcFS 多出来的东西——**真正的多段 walk**。DevFS/ProcFS 是扁平的(根下一层就到底),不需要逐段走;tmpfs 有真实嵌套子目录(`/tmp/build/foo.o`),必须逐段 `find_child`:

```cpp
TmpNode* cur = root_;
while (p[0] != '\0') {
    uint32_t comp_len = 0;
    while (p[comp_len] != '\0' && p[comp_len] != '/') ++comp_len;
    if (cur->inode.type != InodeType::Directory) return Error::NotFound;
    TmpNode* child = find_child(cur, p, comp_len);
    if (child == nullptr) return Error::NotFound;
    cur = child;
    p += comp_len;
    if (p[0] == '/') ++p;
}
```

(`tmpfs.cpp:425-450`,有删节。)中间段必须是目录,否则 `NotFound`(穿过文件往下走,「`x/anything` 但 x 是文件」);末段不存在也 `NotFound`。`lookup_child`(`tmpfs.cpp:458-474`)是给 vfs_lookup 层用的单段入口(同 `find_child` 的锁内包装),让 VFS 的统一路径解析能调进来。

> **一个 C++ 工程坑**。`tmpfs.hpp:60` 的前向声明必须是 `struct TmpNode`,不能是 `class TmpNode`——否则跟 `tmpfs.cpp:19` 的真实定义 tag 不匹配,触发 `-Wmismatched-tags`。而且 `TmpNode` 的定义必须落在 `cinux::fs` 这个具名 namespace 里(`tmpfs.cpp:19`),不能放匿名 namespace——匿名 ns 里的 `struct TmpNode` 是另一个无关类型,`TmpFs::root_`(`tmpfs.hpp:105`)指向的就是不完整类型,直接编译红。这种 tag/namespace 不一致的坑,编译期就能挡住,但报错信息不直观,记一笔省得你踩。
