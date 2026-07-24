---
title: 081 · tmpfs:把可写的临时文件挂进内存
---

# 081 · tmpfs:把可写的临时文件挂进内存

> 想象一个编译场景:GCC 要把 `cc1`、`as`、`ld` 串起来,每一步都往某个临时地方写中间产物——预处理的 `*.i`、汇编排出来的 `*.s`、最后目标文件 `*.o`。这些文件的特点很一致:**可读可写、用完就丢、不该占盘**。Linux 把它们写到 `/tmp`,后面有个叫 tmpfs 的文件系统撑着。可 Cinux 到上一章为止,`/tmp` 还没有着落——根 ext2 倒是能写,但那意味着每写一个临时 `*.o` 都要走 NVMe,慢、还白磨损;更别提那些「编译到一半挂掉、留下的垃圾没人清」的尴尬。
>
> 这一章的真主题,跟同卷 064 DevFS、067 ProcFS 是**一条线上的第三站**——内存型虚拟 FS 这套范式:`FileSystem` 子类 + `InodeOps` 子类 + boot 接线单独一个 `_init.cpp`。DevFS 的内容是「读写触发动作」(`/dev/null` 写全丢),ProcFS 的内容是「现场生成的伪文件」(`cat /proc/<pid>/stat` 拼一段文本)。**tmpfs 跟前两个有一个质的差别:它的内容是真数据**——用户态 `sys_write` 写进去的字节,住在堆上的 `uint8_t* data` 里,能 `mkdir`、能 `touch`、能 `echo` 进去再 `cat` 出来,卸载即消失。同一个模子,但它是这个家族里**第一个「结构可变、内容可写」的成员**。
>
> 做完之后,`/tmp` 真能用:boot 自动挂上、busybox 能在里面建目录写文件、`mount -t tmpfs none /mnt/tmp` 能在运行时再挂一个、`umount` 能把整棵树干净回收。一条诚实的边界先说在前头:这是内存型 FS 的第一刀,只做**最小可用**的 tmpfs——无 inode 级 last-close 语义(`unlink` 立即删 node)、非空目录删除返 EIO 而非 ENOTEMPTY、单 per-FS Spinlock 粗粒度、无 size 上限/无 swap 支撑、卸载即丢、`symlink`/`link`/`rename` 未实现。这些不假装做了,留到正文和「这章没做的」里一条条交代。

## 这章咱们要点亮什么

1. **同一个范式的第三次复用**:`FileSystem` 子类 + `InodeOps` 子类 + boot `_init.cpp` 这个 DevFS/ProcFS 立起来的模子,tmpfs 照搬。差别只在「内容是不是真数据」。
2. **内容存哪:不是生成的,是真在堆上的字节**。`TmpNode` 内嵌 `Inode` + `fs_private` 指回自身(从 DevFS 的「单例 FS 指自己」升级到「per-node 可变内容指自己」),文件内容住在 `data/capacity/size` 三件套里。
3. **写路径三件事**:越 capacity 按 4KB 对齐摊销扩容、gap 零填防 stale 堆字节泄漏、靠 `is_page_cacheable()` 默认 false 这条「故意不做」的逃生路径绕开磁盘 PageCache。
4. **目录树怎么长**:单向兄弟链表代替 DevFS 的定长表,支持运行时 `create`/`mkdir`/`unlink` 改树;`lookup` 是真正的多段 walk(扁平的 DevFS/ProcFS 不需要)。
5. **两条挂载通路**:boot 用静态 `g_tmpfs`、unowned(`umount2` 只摘槽);运行时 `sys_mount` 用堆对象、owned=true(`umount2` 走 `free_tree` 递归回收整棵树)。差别全在挂载表那个 `owned` bool。

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

> **非空目录删除返 EIO,不是 ENOTEMPTY**。Cinux-Base 子模块的 `Error` 枚举没有 `DirectoryNotEmpty` 这一项,所以 `unlink` 一个非空目录返 `Error::IOError`(`tmpfs.cpp:277-279`),syscall 边界映射成 `kEio`。代码注释明说这是已知缺口。**契约层满足**(操作失败,调用方拿到的就是「这事儿干不成」),但 errno 值跟 Linux 不一致——Linux 是 `ENOTEMPTY`。这带来一个测试纪律:测试和教程只能断言「op 失败」(返回非零),**不能断言具体 errno 等于 `ENOTEMPTY`**。这是 ABI/契约分层的一个好教学点:子模块的枚举缺项,会让一个语义清晰的失败,落成一个语义模糊的 errno。留给子模块加枚举项后修。

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

## 两条挂载通路:差别全在挂载表那个 owned bool

tmpfs 有两条挂进系统的路:boot 静态挂 `/tmp`、运行时 `sys_mount` 堆挂。这俩调的是**同一个 `TmpFs` 类、同一个 `mount()`**(幂等,二次 `mount()` 是 no-op,`tmpfs.cpp:375-403`),代码路径几乎对称。**唯一决定「`umount` 时会不会释放」的,是 `vfs_mount_add` 的第三个参 `owned`**。

boot 通路(`tmpfs_init.cpp`):

```cpp
namespace {
TmpFs g_tmpfs;   // 静态局部,寿命 = 整个内核运行
}

bool tmpfs::init() {
    if (!g_tmpfs.mount().ok()) { ... return false; }
    if (!vfs_mount_add("/tmp", &g_tmpfs)) { ... return false; }   // 默认 owned=false
    kprintf("[TMPFS] mounted at /tmp\n");
    return true;
}
```

(`tmpfs_init.cpp:33-50`。)静态 `g_tmpfs` 的命超表——PID1 busybox init、GCC/cc1/as/ld 写中间 `*.o`/`*.s` 全靠 `/tmp`,挂载表只是中途登记一个指针,对象不能随槽位摘除消失(否则 `sys_umount2("/tmp")` 会 `delete` 静态对象 = use-after-free)。所以 `vfs_mount_add` 走默认 2-arg 版,`owned` 默认 false(`vfs_mount.hpp:47`、`vfs_mount.hpp:79`),挂载表只存指针、不管释放。`umount2("/tmp")` 在 `vfs_mount_remove` 里看到 `owned=false`,只摘槽、不 `delete`(`vfs_mount.cpp:93-99`)。

运行时通路(`sys_mount.cpp`):

```cpp
if (strcmp(fstype, "tmpfs") == 0) {
    std::unique_ptr<TmpFs> tfs(new TmpFs());   // 堆对象
    auto m = tfs->mount();
    if (!m.ok()) { ... return -to_errno(m.error()); }
    FileSystem* fs = tfs.release();
    if (!vfs_mount_add(target, fs, /*owned=*/true)) {   // 关键:owned=true
        delete fs;   // 挂载表没收,自己回收
        return -kEnomem;
    }
    return 0;
}
```

(`sys_mount.cpp:51-67`。)`owned=true` 是关键:它告诉挂载表「这对象是你堆分配的、你有释放义务」。`sys_umount2` → `vfs_mount_remove` 看到 `owned=true` 就 `delete fs`(`vfs_mount.cpp:93-95`),`TmpFs` 析构(`tmpfs.cpp:369-373`)递归 `free_tree`(`tmpfs.cpp:351-363`)释放整棵树。错误腿(`mount()` 失败、表满)都有显式 `delete` 回收,不泄漏。

看起来「对象在哪创建」决定生命周期——boot 在静态区、`sys_mount` 在堆上。**实际上决定生命周期的是「挂载表登不登记所有权」**。一个 bool 字段 + 默认参,既不破坏旧的 `owned=false` 调用者(boot 接线、栈局部 mock 的测试),又让 `sys_mount` 路径能干净回收。这是整个挂载表 ownership 模型的设计核心。

> **boot 误标 owned=true 会炸**。boot static 挂载若误传 `owned=true`,`sys_umount2("/tmp")` 会在 `vfs_mount_remove` 里 `delete` 静态 `g_tmpfs` —— 双重释放/崩溃。所以 `tmpfs::init`、`devfs::init`、`procfs::init` 全用默认 2-arg(`owned=false`)是**刻意的安全约束,不是省事**。这条纪律写进挂载表的注释(`vfs_mount.hpp:42-47`):boot/static 接线永远 `owned=false`,只有 `sys_mount` 这种「对象是调用方 new 出来的、没别的 owner」的路径才传 `true`。

**§14 文件门**。`tmpfs.cpp` 是纯逻辑(无 `kprintf`),为的是能同时链进内核和 host 单测;boot 的 `kprintf("[TMPFS] mounted at /tmp")` 落在独立的 `tmpfs_init.cpp`(`tmpfs_init.cpp:48`)。这是 DevFS/ProcFS 同款的 §14 文件门——boot I/O 跟核心逻辑分 TU,源码零 `#ifdef`,CMake 决定编不编。boot 接线在 `proc/init.cpp` 里,紧跟 `procfs::init` 之后(`init.cpp:145`),顺序是 `devfs::init`(`init.cpp:137`)→ `procfs::init`(`init.cpp:141`)→ `tmpfs::init`(`init.cpp:145`)。

## 验证

三层验证,合起来才够。

**第一层:host 无 tmpfs 单测——讲清为啥没有**。`tmpfs.cpp` 虽然是纯逻辑(无 `kprintf`),理论 host 可链,但实际 Book 工作树没给 tmpfs 配 host 单测。原因跟 ProcFS 类似:`tmpfs.cpp` 依赖 `kernel/fs/file.hpp`(`inode_ref`)、`kernel/lib/string.hpp` 这些 kernel-only 路径,host 链进来缺符号。所以 tmpfs 的核心机制测走 kernel harness,不靠 host。这跟 DevFS(有 CharSink 注入缝、host 可测)不一样,跟 ProcFS(直读 registry、host 不可测)更像。

**第二层:kernel 测试,十例机制测 + 一例 syscall 真挂测**。`test_tmpfs.cpp` 的 `TmpFs` section(`test_tmpfs.cpp:287`)十例:mount+root、create/write/read 往返、append+offset read、truncate 缩放、跨 4KB 边界扩容+gap 零填、mkdir+readdir+嵌套 lookup、stat(file+dir)、unlink(文件+空目录/非空目录拒删)、负面路径(缺项/重名/穿文件下走)。测试用**栈局部 `TmpFs`** 直驱 `InodeOps`(`TmpFs tfs; tfs.mount();`),不走挂载表/syscall,所以确定性高、不污染全局 fd 表/挂载表。另有一例 syscall 级真挂测在 `test_vfs_syscall.cpp`:`test_sys_open_creates_tmpfs_file_with_o_creat`(`test_vfs_syscall.cpp:224`)靠 `setup_tmpfs_at_tmp`(`test_vfs_syscall.cpp:80`)把堆 `TmpFs` 真注册进 `/tmp` 全局挂载表(`vfs_mount_add("/tmp", tfs)`,默认 `owned=false`),然后 `do_openat_kernel(O_CREAT)` 建文件、lookup+stat 验内容真在 tmpfs 里。这例证明 tmpfs 经挂载表 + syscall 路径端到端通。

**第三层:make run 冒烟 + busybox smoke**。`make run` 起 QEMU,看 boot 序列出现 `[TMPFS] mounted at /tmp`(`tmpfs_init.cpp:48` 的 kprintf)。进 shell 后用 busybox 做一组操作:`echo hello > /tmp/hello.txt && cat /tmp/hello.txt`(可写可读、字节对得上)、`mkdir /tmp/build && touch /tmp/build/a.o && ls /tmp/build`(目录可建可列)、`rm /tmp/build/a.o && ls /tmp/build`(可删)、`mount -t tmpfs none /mnt/tmp && touch /mnt/tmp/x && umount /mnt/tmp && mount -t tmpfs none /mnt/tmp && ls /mnt/tmp`(运行时挂载/卸载/再挂,文件不见 = owned backend 真 delete)。

> 测试数字怎么填。`run-kernel-test-all` 两腿(单核 + `-smp 2`)的通过数,得在 Book 工作树真跑后填,不照抄源仓库 dev note 的数(那是源仓库的,Book 侧须独立验证)。`scripts/check_test_count.sh` 就是干这个的——基线 `CINUX_TEST_BASELINE` 默认 875(`check_test_count.sh:14`),tmpfs 的 10 例机制测 + 1 例 syscall 真挂测都进 `big_kernel_test`,真跑后 passed 数应 ≥ 基线 + 这些例。用户态真能用 `/tmp` 靠 boot 冒烟 `[TMPFS] mounted at /tmp` + busybox smoke 两腿绿间接证明,不是 `big_kernel_test` 的直接断言——三层证据合力,任一单独都不够。

## 这章没做的

- **无 inode 级 last-close 语义**。`unlink` 摘链后直接 `delete[] data + delete node`,完全不查 inode 的 refcount。内核里其实有个 open-description 引用计数(`Inode.refcount`,`inode_ref`/`inode_unref` 在 `file.cpp`),open fd 持一份。Linux 真语义是「`unlink` 时仍有 open fd 则延迟到 last close 释放」;Cinux 的 tmpfs 不看它,所以**「先 open 再 unlink」会有 use-after-free 风险**。这是诚实简化,依赖 close-before-unlink(GCC 临时文件模式正是如此),真 last-close 语义留 follow-up。
- **非空目录删除返 EIO 而非 ENOTEMPTY**。`Error` 枚举(Cinux-Base 子模块)无 `DirectoryNotEmpty` 项,`unlink` 非空目录返 `Error::IOError` → syscall 边界 `kEio`。契约层满足,errno 不精确,留子模块加枚举项后修。
- **symlink / hardlink / rename 未实现**。`InodeOps` 基类有 `symlink`/`link`/`rename` 这三个虚函数(`inode.cpp:86-97`),默认返 `Error::NotImplemented`;tmpfs 的 `TmpFileOps`/`TmpDirOps` 一个都没覆写,所以落到基类就是 `NotImplemented` → syscall 边界 `kEnosys`。busybox `ln -s /tmp/a /tmp/b`、`mv /tmp/a /tmp/b` 在 `/tmp` 下都会失败。GCC 编译中间产物模式不太依赖这些(rename 原子换名用得少),所以这章先不做,留 follow-up——这是 tmpfs 相对 Linux 的一个明显行为缺口,跟「非空目录返 EIO」是同类已知简化。
- **单 per-FS Spinlock 粗粒度**。一个 `Spinlock`(`tmpfs.hpp:102`)串行化所有树变更 + 内容 I/O。`/tmp` 专用低竞争场景够用,/tmp 上 GCC 多进程高并发未做并发压测。刻意不给 per-node 锁——是为了避开父/子嵌套加锁的 AB-BA 死锁。per-node 锁 / RCU 留 follow-up。
- **无内存上限 / 无 swap 支撑**。tmpfs 理论上可吃光全部堆,本实现无 `size=`/`mode=` 挂载选项、无 `max_blocks` 配额。Linux tmpfs 有 `size=` 挂载选项,Cinux 的没有。`write` 里 `new uint8_t[newcap]` 失败会抛(无 nothrow),kernel 端 new 失败的语义本章不覆盖。swap 回收、oom-kill 全无。
- **MS_* / MNT_* flags 全接受但忽略**。`sys_mount` 的 flags(`MS_NOSUID`/`MS_NODEV`/`MS_NOEXEC`/`MS_RDONLY` 等)注释明写「accepted for Linux ABI parity but not yet modelled」(`sys_mount.cpp:46`);`umount2` 的 `MNT_FORCE`/`MNT_DETACH`/`MNT_EXPIRE` 也是 `[[maybe_unused]]`(`sys_umount2.cpp:22`)。只读挂载、noexec、强卸忙挂载都没建模——挂出来都是可读写可执行。
- **mmap on tmpfs、大文件 offset 溢出防护**未做。
- **/proc/mounts 不存在**。busybox `mount`(无参)列挂载点需要它,ProcFS 动态节点扩展没做,所以 `mount` 命令列不出当前挂载——但挂载本身是生效的(`vfs_resolve` 能命中)。
- **内容不进 PageCache 是设计如此,不是缺陷**——`is_page_cacheable()` 默认 false 是特性。

## 小结

- tmpfs 是内存型虚拟 FS 范式(DevFS 064 / ProcFS 067 已立)的第三次复用:`FileSystem` 子类 + `InodeOps` 子类 + boot `_init.cpp`。差别只在「内容是不是真数据」——tmpfs 的内容是用户态写进去的字节,住在堆上。
- **数据存哪**:`TmpNode` 内嵌 `Inode` + `fs_private` 指回自身,从 DevFS 的「单例 FS 指自己」升级到「per-node 指自己」;文件内容住 `data/capacity/size` 三件套。写路径三件事:4KB 对齐摊销扩容、gap 零填防 stale 堆字节、靠 `is_page_cacheable()` 默认 false 绕开磁盘 PageCache——**正确性来自一个被故意留在默认值的虚函数,不是显式代码**。
- **目录树怎么长**:单向兄弟链表代替 DevFS 的定长表(`first_child`+`next_sibling`,头插,无上限);`make_node` 共用 create/mkdir,`unlink` 带 prev 摘链,`lookup` 真正多段 walk(扁平的 DevFS/ProcFS 不需要)。
- **两条挂载通路**:boot 静态 `g_tmpfs` unowned(`/tmp` 命超表,`umount2` 只摘槽);`sys_mount` 堆对象 owned=true(`umount2` 走 `free_tree` 回收整棵树)。差别全在挂载表那个 `owned` bool——**实际决定生命周期的是「挂载表登不登记所有权」,不是「对象在哪创建」**。
- 诚实边界:无 last-close 语义(`unlink` 立即删,先 open 再 unlink 有 UAF 风险)、非空目录删返 EIO 非 ENOTEMPTY、`symlink`/`link`/`rename` 未实现(返 ENOSYS)、单 per-FS 锁粗粒度、无 size 上限/无 swap、flags 全接受但忽略、`/proc/mounts` 不存在。这些不假装做了,留给后续工程债。
