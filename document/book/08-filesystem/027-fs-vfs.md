---
title: 027 · 给文件一个统一接口:VFS 内核层
---

# 027 · 给文件一个统一接口:VFS 内核层

> 026 让内核能「列出」initrd 里的文件了,但这份能力是死的:你只能打印文件名,没法「打开 `hello.txt`、读它的内容」,更没法对不同的来源(现在是 ramdisk,以后会有磁盘文件系统)用同一套办法操作。这一章,我们在「认得文件」之上搭一层虚拟文件系统(VFS):它定义一个统一的「文件对象」(inode)、一套统一的操作接口、一张「路径 → 文件系统」的挂载表,然后让 ramdisk 实现这套接口挂上来。完成后,内核拿一个路径,就能解析出对应的文件对象、读出内容——不管它背后是 ramdisk 还是别的东西。注意:这一章是**内核内部**的管线,用户态还碰不到它(那要等 027b 接上系统调用)。027 内容多,拆成两篇,这是第一篇。

## 这一章我们要点亮什么

内核第一次有了「VFS」这个词背后那套结构。具体四块:

第一,一个**文件对象的抽象**——`Inode`。它代表「一个文件」(或目录),带着编号、大小、类型,以及一组「怎么操作我」的函数指针。

第二,一套**文件系统后端要遵守的接口**——`FileSystem`。任何一个想被 VFS 管理的文件系统(ramdisk、将来的 ext2),都得实现「挂载」和「按名查找」两个方法。

第三,一张**挂载表**——把「路径前缀」映射到「哪个文件系统」。内核拿到一个绝对路径,靠这张表找到该交给哪个后端处理。

第四,一个**「打开的文件」描述**和**文件描述符表**——`File` 记录「这个打开的文件,读写到哪个偏移了」,`FDTable` 管理一个进程手里的所有打开文件(占位,fd 编号就是它的下标)。

验收点是内核启动时打出的:`[VFS] Ramdisk mounted at /`——ramdisk 被挂到了根路径 `/`,之后内核就能通过 VFS 按名找到 initrd 里的文件、读出内容。

## 为什么现在需要它

为什么紧跟在 026 之后。026 的 ramdisk 只会「解析归档、打印清单」,它的 `mount` 返回个文件数就完事了,文件内容谁都拿不到。问题出在缺一层抽象:没有「按名字取文件」的接口,没有「把名字变成可读对象」的机制。要往「能加载并运行磁盘上的程序」走,内核必须能 `lookup("hello.txt")` 拿到一个能读的对象。这一章就是把这层抽象立起来。

为什么要做成「虚拟」文件系统,而不是让 ramdisk 直接提供读写?因为文件来源会变。今天只有 ramdisk(只读、内存里),028 会来 ext2(磁盘上、能读写)。如果上层(系统调用、shell)直接调 ramdisk 的函数,等 ext2 来了就得改两遍。VFS 的价值就在这:它定义一套**与后端无关**的接口(inode + 操作表),上层只跟 VFS 打交道,后端各自实现这套接口。换个文件系统,上层一行不改。这就是 Linux 也用的那套思路——我们这一章是它的最小形态。

还有一笔设计上的账,关于「怎么实现多态」。一个文件系统后端要提供「读、写、列目录」等操作,而每个文件对象(inode)都得能调到这些操作。最直觉的 C++ 做法是给 inode 加虚函数,但那是**每个 inode 一个虚表指针**——文件一多,这笔开销不小。这一章用了个更省的手法:把操作做成一个**函数指针表**(`InodeOps`),同一类 inode 共用一张静态表,inode 里只存一个指向表的指针。这避免了每对象虚表,代价是函数指针表要手工维护(下面专门讲)。而文件系统**这一层**(只有 ramdisk、ext2 这么几个后端)反而用了正经的 C++ 虚函数——后端数量少,虚表开销无所谓。这种「对象多用函数指针表、类型少用虚函数」的混搭,是这一章一个值得记住的设计取舍。

> **作者事后吐槽**:是的,这个「值得记住的设计取舍」后来被作者自己推翻了。等到 028b 那一串做 ext2 的写入/创建时,操作从三个(read/write/readdir)涨到一长串(create、mkdir、unlink、stat…),`InodeOps` 索性直接长成了带 `virtual` 的类——函数指针表当初省下来的那点内存,在「方法越来越多、还要继承要重写」面前根本不值一提。所以别把这里的取舍当成永恒真理:它只是 027 这个阶段、就这三个方法时的合理选择。这一章我们照 027 的原貌讲(它就是函数指针表),但权当个预告——抽象会演化,别过早把它焊死。

## 设计图

VFS 把「一个路径」变成「一次读」,中间经过几层。看完整链路:

```text
   用户/上层拿一个路径 "/hello.txt"
        ▼
   vfs_resolve(path, &rel_path)          ← 查挂载表,最长前缀匹配
        │   遍历 MountPoint[]:找 path 的最长前缀
        │   边界判定:匹配必须落在路径分量边界(后跟 '/' 或 '\0')
        ▼
   FileSystem* fs  +  rel_path("hello.txt")   ← 剥掉挂载前缀后的相对路径
        ▼
   fs->lookup(rel_path)                      ← 后端按名找,返回 Inode*
        ▼
   Inode { ino, size, type, ops, fs_private }
        │   ops 指向静态 InodeOps 表
        ▼
   inode->ops->read(inode, offset, buf, count)   ← 通过函数指针调到后端的读
        │   后端的 read 用 inode->fs_private 找回自己的私有数据(如 RamdiskEntry)
        ▼
   数据进 buf
```

打开文件状态(`File` / `FDTable`)是这条链路之外的另一条线,管「这次打开,读到哪了」:

```text
   sys_open(将来):resolve+lookup 得到 inode → FDTable.alloc(inode,flags) → 返回 fd
   FDTable[fd] = File{ inode, offset=0, flags }
        │   多个 fd 可指向同一 inode,各自 offset 独立
   sys_read(将来):fd → FDTable.get(fd) → file->inode->ops->read(inode, file->offset, ...)
                  → 读到 buf,file->offset += n
```

两条线在「inode」上汇合:VFS 链路负责「路径 → inode」,FDTable 链路负责「inode + 打开状态 → 多次读写」。这一章两条线都搭好(只是还没接系统调用)。

## 代码路线

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

0/1/2 留给 stdin/stdout/stderr——这是 Unix 的老规矩,shell 那边(027b)会用到。这一章还只有一张**全局**的 FDTable(`g_global_fd_table()`),不是每进程一张;头文件注释明说了「later per-process」——等进程隔离成熟了再拆。如实说,别拔高成每进程独立。

### ramdisk 变成一个真正的文件系统

前面三块是 VFS 的「骨架」(inode、FileSystem、mount 表、File/FDTable),都是接口和容器。现在让 ramdisk 长上肉——它继承 `FileSystem`,实现 `mount`/`lookup`,并提供 `InodeOps`。

`mount` 不再只是打印清单,而是**建一张条目表**,每个文件占一项,顺带把它的 inode 预分配好([ramdisk.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ramdisk.cpp)):

```cpp
bool Ramdisk::mount() {
    // ... 解析 ustar 归档边界、遍历头(和 026 一样)...
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

注意这里的诚实边界:lookup 是**扁平**的线性搜,把整个 ramdisk 当成一个目录——它不解析多级路径(没有「进 etc/ 再找 passwd」的逐级遍历,而是直接拿 `etc/passwd` 这种带 `/` 的名字整体比)。`readdir` 也一样:它把整盘当一个目录,先吐 `.`、`..`,再依次吐所有文件条目。这对「initrd 就是一堆平铺文件」够用,但别拔成层级目录树——那要等 028 真文件系统。

## 调试现场

027 没有 notes 文件,但 VFS 这套类型擦除 + 前缀匹配有几个经典坑,值得当调试现场。

一是 **最长前缀匹配漏了边界判定**。`vfs_resolve` 光用 `strncmp` 比前缀,挂载点 `/fo` 会误匹配 `/foo`、`/etc` 误匹配 `/eternity`。症状是「明明挂的是 `/`,查 `/foo` 却解析到了错误的后端」或「多个挂载点时路由到错的那个」。那道 `path[mlen]` 必须是 `/` 或 `\0`(或挂载前缀本身以 `/` 结尾)的判定不能省——它把「字符串前缀」收窄成「路径分量前缀」。

二是 **`fs_private` 没接上**。`ramdisk_read` 靠 `inode->fs_private` 找回 `RamdiskEntry`。如果 `mount` 建条目时忘了 `entry.inode.fs_private = &entry`,或者类型擦除的 `static_cast` 接错了结构,read 读到的全是垃圾或直接返回 -1(函数里的 null 检查兜底)。这是类型擦除设计的固有成本:私有数据的串联全靠后端自觉,编译器不帮你查类型。建 inode 时,`ops` 和 `fs_private` 必须成对设对。

三是 **ramdisk 不是只读就乱写**。`ramdisk_write` 恒返回 -1。如果有上层(比如将来的 sys_write 通过 fd)没检查 write 的返回值、以为写成功了,数据其实没落盘(本来也落不了——归档在内核镜像里)。ramdisk 的定位就是只读内存盘,写操作一律失败,调用方必须处理这个 -1。

四是 **`Ramdisk` 对象的生命周期**。`main.cpp` 里它是 `static cinux::fs::Ramdisk ramdisk;`——`static` 不是装饰。挂载表里存的是 `&ramdisk`,而所有 inode 都嵌在 `ramdisk.entries_[]` 里、根 inode 在 `ramdisk.root_inode_`。如果 ramdisk 是个栈上局部、函数返回就析构,VFS 表里那堆 `Inode*` 全成了野指针,后续 lookup/read 必崩。凡是「对象内部数据被外部以裸指针引用」的情况,这个对象的生命周期必须长于所有引用——这里靠 `static` 保活。

五是 **FDTable 从 fd 3 起算**。`alloc` 跳过 0/1/2。如果忘了这规矩、从 0 开始分配,新打开的文件会抢掉 stdin(0)/stdout(1)/stderr(2) 的位置,后续往这些 fd 写就写进了普通文件而不是终端——shell 的输出全乱。0/1/2 是 Unix 给标准流留的,这一章先把坑占住。

## 验证

VFS 的逻辑能在 host 上镜像测一部分。挂载表的 resolve(最长前缀、边界判定、add/remove)和 FDTable(alloc 从 3 起、close、get、表满)是纯数据结构操作,host 直接测:[test_vfs_mount.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_vfs_mount.cpp) 验 resolve 的前缀/边界/最长匹配,[test_fd_table.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_fd_table.cpp) 验描述符分配回收。

```bash
ctest --test-dir build -R 'vfs_mount|fd_table' --output-on-failure
```

「真归档、真挂载、真 lookup/read」在 QEMU 里验。机内测 [test_ramdisk.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_ramdisk.cpp) 在 026 基础上扩了一大块(这一章加了三百多行):验 `mount` 建出条目表、`lookup` 按名找到 inode、通过 `inode->ops->read` 读出文件内容、`readdir` 列出条目。跑它:

```bash
cmake --build build --target run-kernel-test
```

或直接跑完整内核,验收点是 Step 27 打出的 `[VFS] Ramdisk mounted at /`,以及 ramdisk 列出的文件清单——看到这两样,说明「挂载表 + ramdisk 实现 FileSystem + inode 可读」整条内核管线通了。这一章的难点和前两章一样:VFS 正确性靠现象间接验证(能 lookup 到、能读出内容),所以 host 数据结构测(焊死 resolve/FDTable)+ 机内测(真跑 ramdisk 全链路)缺一不可。

## 下一站

到这里,内核内部已经有了一套完整的 VFS 管线:挂载表把路径交给后端、后端 lookup 出 inode、inode 通过操作表能读写。但这一切都**只存在于内核里**——用户态的程序(shell)碰不到它,没有系统调用能 `open` 一个文件。VFS 现在像一间装修好却没开门的房间。

下一站(027b),我们给这间房开门:把 VFS 接到系统调用上——`sys_open`(路径解析 + 分配 fd)、`sys_read`/`sys_write`(通过 fd 找 File、调 inode 操作)、`sys_close`、`sys_getdents`(列目录)。再配上用户态 libc 的封装和 shell 的 `cat`/`ls` 命令,用户就能在 shell 里敲 `cat hello.txt`、`ls`,真正用上这一章搭的 VFS。不过那是下一篇的事,我们先把「内核有了 VFS」这个里程碑坐实。

---

### 参考

- Linux VFS 设计(`Documentation/filesystems/vfs.txt`、内核源码 `fs/*.c`):inode / 超级块 / file_operations / dentry 这套抽象是「inode + 操作表 + 挂载」思路的成熟形态,本章是它的最小化对照——用来理解设计取舍,**不**代表 Cinux 实现了 Linux 那套(dentry cache、inode cache、page cache 本章都没有)。
- 026 章 · [内核第一次认识「文件」:嵌入式 initrd ramdisk](026-fs-ramdisk.md):本章 ramdisk 在 026 的「解析 ustar」基础上,加了 FileSystem 接口、条目表和 inode,两章是文件系统卷的前两阶。
- 本 tag 源码:[inode.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/inode.hpp)、[vfs_filesystem.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/vfs_filesystem.hpp)、[vfs_mount.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/vfs_mount.hpp) / [vfs_mount.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/vfs_mount.cpp)、[file.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/file.hpp) / [file.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/file.cpp)、[ramdisk.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ramdisk.hpp) / [ramdisk.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ramdisk.cpp)(本章 ramdisk 实现 FileSystem)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp)(Step 27 挂载);测试 [test_vfs_mount.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_vfs_mount.cpp)、[test_fd_table.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_fd_table.cpp)、[test_ramdisk.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_ramdisk.cpp)(本章扩了 lookup/read/readdir)。
