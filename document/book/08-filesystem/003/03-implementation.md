---
title: 03 · 实现:inode、FileSystem、挂载表、FDTable、ramdisk
---

# 实现:inode、FileSystem、挂载表、FDTable、ramdisk

### inode:一个文件对象的抽象 + InodeOps 手工虚表

VFS 的核心数据结构是 inode。它代表一个文件(或目录),见 [inode.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/inode.hpp):

```cpp
enum class InodeType : uint8_t { Unknown=0, Regular=1, Directory=2 };

struct InodeOps {
    int64_t (*read)(const Inode* inode, uint64_t offset, void* buf, uint64_t count);
    int64_t (*write)(Inode* inode, uint64_t offset, const void* buf, uint64_t count);
    int64_t (*readdir)(const Inode* inode, uint64_t index, char* name, uint64_t name_max);
};

struct Inode {
    uint64_t  ino;          // 文件系统自己定的编号
    uint64_t  size;         // 文件大小
    InodeType type;         // Regular / Directory
    InodeOps* ops;          // 操作函数表(可空)
    void*     fs_private;   // 后端私有数据的不透明指针
};
```

`InodeOps` 是这一章的灵魂。它是个**函数指针表**:read、write、readdir 各占一槽。一个 inode 的 `ops` 指向某张静态表,调用 `inode->ops->read(...)` 就是通过函数指针跳到后端的具体实现。这正是上面说的「手工虚表」——它达到了多态的效果(不同后端的 inode,read 行为不同),但每个 inode 只花一个指针(`ops`),没有 C++ 虚函数那种每对象一个隐藏虚表指针。

`fs_private` 是配合它的关键。函数指针表的 `read` 函数签名只收一个 `Inode*`,但后端需要自己的私有数据(比如 ramdisk 要知道「这个文件的数据在归档的哪里」)。怎么办?inode 里留一个 `fs_private` 不透明指针,后端在创建 inode 时把它指向自己的私有结构(`RamdiskEntry*`),`read` 实现里再 `static_cast` 回来:

```cpp
int64_t ramdisk_read(const Inode* inode, uint64_t offset, void* buf, uint64_t count) {
    auto* entry = static_cast<const RamdiskEntry*>(inode->fs_private);  // 找回私有数据
    // ... 从 entry->data + offset 拷贝 ...
}
```

这种「函数指针表 + 不透明私有指针」是 C 风范的类型擦除,好处是省内存、跨后端统一;代价是私有数据的串联全靠后端自己维护,接错就崩(调试现场会讲)。

### FileSystem:后端要实现的两个方法

文件系统这一层用正经的 C++ 虚函数。任何后端都得继承 `FileSystem`([vfs_filesystem.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/vfs_filesystem.hpp)),实现两个纯虚方法:

```cpp
class FileSystem {
public:
    virtual bool mount() = 0;          // 挂载:初始化后端,建好内部结构
    virtual Inode* lookup(const char* path) = 0;  // 按相对路径找一个文件
};
```

为什么这里用虚函数、InodeOps 用函数指针?数量决定。文件系统后端就那么几个(ramdisk、ext2…),每个后端一个虚表,开销可忽略;而 inode 会有很多,不能每个都背虚表。所以「类型少」的层级用虚函数,「对象多」的层级用函数指针表——这是个干净的分工。

VFS 的上层(mount 表、系统调用)只持有 `FileSystem*`,调 `mount`/`lookup` 时走虚表派发到具体后端,完全不知道后端是 ramdisk 还是别的。

### mount 表:路径前缀怎么找到后端

内核可能同时挂着好几个文件系统,各自管一段路径。这张映射表就是 mount 表([vfs_mount.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/vfs_mount.hpp) / [vfs_mount.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/vfs_mount.cpp)):

```cpp
static constexpr uint32_t MOUNT_TABLE_SIZE = 8;   // 最多 8 个挂载点
struct MountPoint {
    char        path[MOUNT_PATH_MAX];   // 绝对路径前缀,如 "/"
    FileSystem* fs;                      // 对应后端
    bool        in_use;
};
static MountPoint g_mount_table[MOUNT_TABLE_SIZE];
```

`vfs_mount_add(path, fs)` 找空槽塞进去;`vfs_resolve(path, &rel_path)` 是核心——它遍历整张表,找出 path 的**最长前缀**匹配,返回那个后端,并把 `rel_path` 指到「剥掉挂载前缀后的相对路径」:

```cpp
FileSystem* vfs_resolve(const char* path, const char** rel_path) {
    FileSystem* best_fs = nullptr;
    uint32_t best_len = 0;
    for (...) {
        // path 是否以这个挂载前缀开头?
        if (strncmp(mpath, path, mlen) != 0) continue;
        // ★ 边界判定:匹配必须落在路径分量边界
        if (mpath[mlen-1] != '/') {
            if (path[mlen] != '\0' && path[mlen] != '/') continue;   // "/fo" 不能匹配 "/foo"
        }
        if (mlen > best_len) { best_len = mlen; best_fs = g_mount_table[i].fs; }
    }
    if (best_fs) *rel_path = path + best_len;
    return best_fs;
}
```

那段边界判定是命门。光做 `strncmp` 前缀匹配不够:挂载点 `/fo` 会错误地匹配路径 `/foo`,因为 `/foo` 确实以 `/fo` 开头。正确的做法是确认匹配点落在「路径分量边界」上——要么挂载前缀本身以 `/` 结尾(如 `/`,天然是边界),要么 path 里前缀之后的下一个字符是 `/` 或 `\0`。加上这道闸,`/fo` 才不会误匹配 `/foo`。最长前缀则保证了「更具体的挂载点优先」:如果 `/` 和 `/mnt` 都挂了东西,查 `/mnt/x` 会落到 `/mnt` 那个后端。

这一章的 `main.cpp` 启动时挂的就是根路径:

```cpp
cinux::fs::vfs_mount_init();
cinux::fs::vfs_mount_add("/", &ramdisk);   // 整个根都交给 ramdisk
```

### File 与 FDTable:打开的文件状态

「找到 inode」是一次性的事,但「读写一个打开的文件」是有状态的——你得记住读到哪个偏移了。`File` 就是这个状态([file.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/file.hpp)):

```cpp
struct File {
    Inode*    inode;    // 指向底层 inode
    uint64_t  offset;   // 当前读写偏移
    OpenFlags flags;    // RDONLY / WRONLY / RDWR
};
```

注意 `File` 把 inode 和 offset **分开存**:同一个 inode 可以被多个 `File` 打开(比如两次 open 同一个文件),各自维护独立的 offset——互不干扰。这就是「打开文件描述」(open file description)和「inode」分离的经典设计。

`FDTable` 是「文件描述符表」,管一组 `File*`:

```cpp
static constexpr uint32_t FD_TABLE_SIZE = 256;
static constexpr int FD_NONE = -1;
class FDTable {
    File* fds_[FD_TABLE_SIZE];
public:
    int alloc(Inode* inode, OpenFlags flags);   // 找空槽,塞个新 File,返回 fd
    int close(int fd);                           // 释放
    File* get(int fd) const;                     // 按 fd 取 File
};
```

`alloc` 有个细节:它从 **fd 3** 开始分配,跳过 0、1、2:

```cpp
static constexpr uint32_t FD_FIRST = 3;   // 0=stdin, 1=stdout, 2=stderr 预留
int FDTable::alloc(Inode* inode, OpenFlags flags) {
    for (uint32_t i = FD_FIRST; i < FD_TABLE_SIZE; ++i) {
        if (fds_[i] == nullptr) { fds_[i] = new File{inode, 0, flags}; return i; }
    }
    return FD_NONE;
}
```

0/1/2 留给 stdin/stdout/stderr——这是 Unix 的老规矩,shell 那边(004)会用到。这一章还只有一张**全局**的 FDTable(`g_global_fd_table()`),不是每进程一张;头文件注释明说了「later per-process」——等进程隔离成熟了再拆。如实说,别拔高成每进程独立。

### ramdisk 变成一个真正的文件系统

前面三块是 VFS 的「骨架」(inode、FileSystem、mount 表、File/FDTable),都是接口和容器。现在让 ramdisk 长上肉——它继承 `FileSystem`,实现 `mount`/`lookup`,并提供 `InodeOps`。

`mount` 不再只是打印清单,而是**建一张条目表**,每个文件占一项,顺带把它的 inode 预分配好([ramdisk.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ramdisk.cpp)):

```cpp
bool Ramdisk::mount() {
    // ... 解析 ustar 归档边界、遍历头(和 002 一样)...
    if (type == UstarType::REGULAR || type == UstarType::CONTIGUOUS) {
        auto& entry = entries_[entry_count_];
        // 拷贝文件名、记大小、记数据指针(指向归档内那块数据)
        entry.size = file_size;
        entry.data = base_ + offset + sizeof(UstarHeader);
        // ★ 预分配这个文件的 inode
        entry.inode.ino       = entry_count_;
        entry.inode.size      = file_size;
        entry.inode.type      = InodeType::Regular;
        entry.inode.ops       = &ramdisk_file_ops;       // 指向静态操作表
        entry.inode.fs_private = &entry;                  // 私有数据指回自己
        ++entry_count_;
    }
    // ... 最后建根目录 inode(支持列目录)...
    root_inode_.type = InodeType::Directory;
    root_inode_.ops  = &ramdisk_dir_ops;
    root_inode_.fs_private = &root_ctx_;   // root_ctx_ 指向整张条目表
    return entry_count_ > 0;
}
```

几个要点。第一,每个文件的 inode **在挂载时就预分配好**、嵌在 `RamdiskEntry` 里(不是每次 lookup 现场造),`fs_private` 指回这个 entry——这样 `ramdisk_read` 能通过 inode 找到「数据在哪、多大」。第二,操作表是**静态**的、按类型分两张:`ramdisk_file_ops`(有 read、write 返回 -1、无 readdir)给普通文件,`ramdisk_dir_ops`(无 read、有 readdir)给目录。同一类 inode 共用一张表,这正是「函数指针表省内存」的体现。第三,ramdisk 是**只读**的——`ramdisk_write` 恒返回 -1,因为数据是嵌在内核镜像里的归档,没法写。这是个硬边界,别当成能写的文件系统。

`lookup` 按名找:根路径(`/`)返回根目录 inode,否则剥掉前导 `/` 后在条目表里线性搜:

```cpp
Inode* Ramdisk::lookup(const char* path) {
    if (path[0]=='\0' || (path[0]=='/' && path[1]=='\0')) return &root_inode_;
    if (path[0]=='/') ++path;
    for (uint32_t i = 0; i < entry_count_; ++i) {
        // 逐字节比 path 和 entries_[i].name,全等则返回该 inode
    }
    return nullptr;
}
```

注意这里的诚实边界:lookup 是**扁平**的线性搜,把整个 ramdisk 当成一个目录——它不解析多级路径(没有「进 etc/ 再找 passwd」的逐级遍历,而是直接拿 `etc/passwd` 这种带 `/` 的名字整体比)。`readdir` 也一样:它把整盘当一个目录,先吐 `.`、`..`,再依次吐所有文件条目。这对「initrd 就是一堆平铺文件」够用,但别拔成层级目录树——那要等 005 真文件系统。

## 调试现场
