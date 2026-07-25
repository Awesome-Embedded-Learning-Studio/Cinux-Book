---
title: 085 · mount/umount:把后端接到路径上的接线口
---

# 085 · mount/umount:把后端接到路径上的接线口

> 想象一句最常见的运维动作:`sys_mount -t ext2 /dev/sda /mnt`——把一块盘挂到一个路径前缀下,之后 `vfs_resolve("/mnt/...")` 命中它。boot 时 `/`、`/tmp`、`/proc`、`/dev` 这几条静态接线已经挂好;可一旦到了运行时,「再挂第二块盘到 `/mnt`」「`umount /mnt` 把它卸下来」——这两件事的内部长什么样,到上一章为止还是个黑盒。081 tmpfs 章已经把 `sys_mount` 的 tmpfs 分支拆开过(堆 `new TmpFs` + `owned=true` 那条路),079 VFS 收尾章把 `vfs_resolve` 跨挂载点、flock、dentry cache 都讲完——可那里明确标了 deferred 的「`sys_mount` 怎么把一块**真块设备**挂上来」,就留给了这一章。
>
> 这章拆开的就是 `sys_mount` 这个**接线口**。它本身不造数据,只把「一块已经存在的后端 FileSystem」绑到「一个路径前缀」上——`vfs_resolve` 走最长前缀匹配落表,那是 079 讲过的消费侧;本章看的是**表的填充侧**:`sys_mount` 怎么往挂载表里塞东西。塞法不是一个,是四种:`fstype` 字符串当 selector,工厂方法按字符串分派——tmpfs 堆分配独占、proc/devfs 取 boot 单例共享、ext2/ext4 依赖外部块设备造、未知 fstype 一律 `ENODEV`。
>
> 做完之后,`sys_mount -t ext2 /dev/sda /mnt` 真能挂上一块盘,`umount2 /mnt` 能干净回收;`MS_RDONLY` 之类的 flags 收下但忽略、忙挂载不返 `EBUSY`、没有 `/proc/mounts` 可查表——这些边界先说在前头,以源码为准,不假装做了。

## 这章咱们要点亮什么

1. **mount factory 是 fstype 驱动的四类分发**——`do_mount_kernel` 按 `fstype` 字符串走路由:tmpfs / proc·devfs / ext2·ext4 / 未知返 `ENODEV`。跟 081 tmpfs 章是**同一张表的不同行**:081 只把 tmpfs 那一行展开了,这里看全表。
2. **三类 owned 策略在一个 bool 上收敛**(承 081)——堆 `owned=true` / 单例 `owned=false` / 未知 `ENODEV`,全在 `MountPoint.owned` 这一个字段上。
3. **块设备挂载链是 079 deferred 的核心**——source 路径 → `vfs_lookup(NoFollow)` 拿 `Inode` → `InodeOps::block_device()` 虚方法抽 `IBlockDevice` → `new Ext2(dev)`,三层解析把一个字符串变成一个挂好的 FS。
4. **四个 errno 的精确分工**——`EINVAL`(参数缺)/ `ENOENT`(source 路径不存在)/ `ENXIO`(source 解析得通但不是块设备)/ `ENODEV`(未知 fstype 或单例未 init)。
5. **诚实边界——flags accepted but ignored**——`MS_*` / `MNT_*` 全收下不解析,`/proc/mounts` 没有,忙挂载不返 `EBUSY`,`BlockDevOps` 不支持裸盘读写。这些是工程折中,不是漏。

## mount factory:四类 fstype 怎么分发

把 `do_mount_kernel` 当一个**工厂方法**看:输入三个字符串(`source`/`target`/`fstype`)+ 一个 `flags`,输出一个挂在挂载表里的 `FileSystem`,或者一个 errno。先把签名和入口校验摆出来:

```cpp
int64_t do_mount_kernel(const char* source, const char* target, const char* fstype, uint64_t flags) {
    // MS_* flags are accepted for Linux ABI parity but not yet modelled.
    if (target == nullptr || fstype == nullptr || target[0] == '\0') {
        return -kEinval;
    }
    // ...
}
```

(`sys_mount.cpp:45-49`。)`target` 和 `fstype` 是必填(没 target 挂哪、没 fstype 不知道造什么 FS),`source` 只对 ext2/ext4 必填(tmpfs/proc/devfs 不需要后端设备)。`flags` 收下但全局注释明说「accepted for Linux ABI parity but not yet modelled」——这一行先记下,边界节再展开。

接下来四类分支,贴前两类(tmpfs + proc/devfs)看全貌:

```cpp
// ---- tmpfs: heap-allocated per mount; owned (sys_umount2 frees the tree) ----
if (strcmp(fstype, "tmpfs") == 0) {
    std::unique_ptr<TmpFs> tfs(new TmpFs());
    auto                   m = tfs->mount();
    if (!m.ok()) { /* ... */ return -to_errno(m.error()); }
    FileSystem* fs = tfs.release();
    if (!vfs_mount_add(target, fs, /*owned=*/true)) {
        delete fs;  // mount table did not take ownership; reclaim
        return -kEnomem;
    }
    return 0;
}

// ---- proc / devfs: boot singletons.  Mounting at a second path shares the
// one instance; owned=false means sys_umount2 detaches the mount point but
// never frees the singleton (it outlives any one mount point, as at /proc).
if (strcmp(fstype, "proc") == 0 || strcmp(fstype, "devfs") == 0) {
    FileSystem* fs = (strcmp(fstype, "proc") == 0)
                         ? static_cast<FileSystem*>(cinux::fs::procfs::instance())
                         : static_cast<FileSystem*>(cinux::fs::devfs::instance());
    if (fs == nullptr) {
        return -kEnodev;  // the FS was never initialised at boot
    }
    if (!vfs_mount_add(target, fs, /*owned=*/false)) { /* ... */ return -kEnomem; }
    return 0;
}
```

(`sys_mount.cpp:52-84`,有删节——省略了 `kprintf` 错误日志。)tmpfs 那一行 081 章讲透了——堆 `new TmpFs`、`mount()` 成功才 `release()` 交裸指针给挂载表、`owned=true` 让 `umount2` 走 `free_tree` 回收整棵树。这里只贴代码回顾链路,RAII 细节不重讲。

proc/devfs 这一行最值得停下来看。它**不 new 任何东西**——取的是 boot 单例:

```cpp
FileSystem* fs = (strcmp(fstype, "proc") == 0)
                     ? static_cast<FileSystem*>(cinux::fs::procfs::instance())
                     : static_cast<FileSystem*>(cinux::fs::devfs::instance());
if (fs == nullptr) {
    return -kEnodev;  // the FS was never initialised at boot
}
```

(`sys_mount.cpp:73-78`。)`procfs::instance()`(`procfs.hpp:147`)和 `devfs::instance()`(`devfs.hpp:223`)是两个 boot 单例的访问器。它们的实现都长这样:**没 init 过就返 `nullptr`**——`ProcFs::is_mounted()`(`procfs.hpp:97`,看 `mounted_` flag)、`DevFs::is_mounted()`(`devfs.hpp:139`,看 `node_count_ > 0`)。工厂看到 `nullptr` 直接返 `-kEnodev`,**拒挂半个未初始化的单例**。这是工厂方法的一个重要纪律:产品还没造好,工厂不发货。

第四类兜底分支:

```cpp
// ---- ramfs (no such FS) / fat / xfs / ... : not supported.  Linux returns
// ENODEV for an unknown filesystem type.
(void)source;
(void)flags;
kprintf("[SYS_MOUNT] unknown filesystem type '%s'\n", fstype);
return -kEnodev;
```

(`sys_mount.cpp:123-128`。)`ramfs` 在 Cinux 压根不存在,`fat`/`xfs` 这些超范围,统一 `-kEnodev`——这是 Linux `mount(2)` 对未知 fstype 的语义。

> **fstype 字符串就是工厂 selector**。把「选哪个 FS」从「怎么造 FS」里剥出来,是这套设计的核心抽象:调用方只扔一个字符串进来,工厂内部分派到四条腿,每条腿自己决定是 new、是取单例、是走块设备链、还是返错。日后加新 FS(nfs 之类),只在工厂里加一个 `strcmp` 分支就行,不用动调用方。这是「工厂方法」的常见误解:工厂不一定每次都造新对象,**取共享单例也是合法的工厂产品**——`procfs::instance()` 就是这个 SingletonRegistry 的味道。`owned=false` 是这个共享语义在挂载表层的一个 bool 投影:挂载表不拥有这个对象,只登记一个指针。

## 块设备挂载链:从 /dev/sda 到 Ext2 实例

这一节是本章的**独特核心**——079 VFS 收尾章明确 deferred 的「`sys_mount` 怎么挂一块真盘」,就在这 30 行里。完整贴出来:

```cpp
// ---- ext2 / ext4: source is a block-device path (e.g. /dev/sda).  Resolve
// it to an Inode, pull the IBlockDevice via block_device(), and mount a fresh
// Ext2 over it.  NoFollow: /dev/sda is the device node itself, not a symlink
// to follow.  ENXIO (Linux) when source is not a block device.
if (strcmp(fstype, "ext2") == 0 || strcmp(fstype, "ext4") == 0) {
    if (source == nullptr || source[0] == '\0') {
        return -kEinval;
    }
    auto lr = cinux::fs::vfs_lookup(source,
                                    static_cast<uint32_t>(cinux::fs::LookupFlag::NoFollow), "/");
    if (!lr.ok()) {
        return -to_errno(lr.error());
    }
    cinux::fs::Inode*                ino = lr.value().target;
    cinux::drivers::IBlockDevice*    dev =
        (ino != nullptr && ino->ops != nullptr) ? ino->ops->block_device(ino) : nullptr;
    if (ino != nullptr) {
        cinux::fs::inode_unref(ino);  // vfs_lookup returned a ref
    }
    if (dev == nullptr) {
        return -kEnxio;  // source resolves, but is not a block device
    }
    std::unique_ptr<cinux::fs::Ext2> ext2(new cinux::fs::Ext2(dev));
    auto                             m = ext2->mount();
    if (!m.ok()) { /* ... */ return -to_errno(m.error()); }
    FileSystem* fs = ext2.release();
    if (!vfs_mount_add(target, fs, /*owned=*/true)) { /* ... */ delete fs; return -kEnomem; }
    return 0;
}
```

(`sys_mount.cpp:90-121`,有删节——省略了 `kprintf` 错误日志。)拆成三层。

**第一层:`vfs_lookup(source, NoFollow)` 把字符串变成 `Inode`。** `/dev/sda` 这个路径,走标准的 VFS 路径解析(就是 079 章讲的那套 `vfs_lookup`),拿到带 ref 的 `Inode*`。这里 `NoFollow` 这个 flag 是关键——`LookupFlag::NoFollow = 1u << 5`(`vfs_lookup.hpp:39`),它的语义是「**末段是 symlink 也不要 follow**」。`/dev/sda` 是设备节点本身,不是软链;但语义上必须标 `NoFollow`,否则 Follow 模式下若末段偶为 symlink(想象有人把 `/dev/sda` 做成指向别处的软链),会被解析到非块设备目标,`block_device()` 返 `nullptr`,误判 `ENXIO`。和 `readlink`/`lstat` 用 `NoFollow` 同一原理:**要看节点本身,不看它指向哪**。

**第二层:`ino->ops->block_device(ino)` 调 `InodeOps` 虚方法抽 `IBlockDevice`。** 这一步是整条链最反直觉的设计。把 `IBlockDevice*` 暴露给 `sys_mount` 的,不是某个块设备 API,而是 `InodeOps` 的一个**虚槽**:

```cpp
cinux::drivers::IBlockDevice* dev =
    (ino != nullptr && ino->ops != nullptr) ? ino->ops->block_device(ino) : nullptr;
if (ino != nullptr) {
    cinux::fs::inode_unref(ino);  // vfs_lookup returned a ref
}
if (dev == nullptr) {
    return -kEnxio;  // source resolves, but is not a block device
}
```

(`sys_mount.cpp:100-107`。)`vfs_lookup` 返回的 `Inode` 带一份引用计数(`inode_ref`),这里必须 `inode_unref` 平账——否则引用计数泄漏,块设备节点的 inode 永远释放不掉。拿到 `dev` 之后判空:`nullptr` 就返 `-kEnxio`。

**第三层:`new Ext2(dev)` 把 `IBlockDevice` 喂给 Ext2 构造,真跑 `mount()` 读超级块。**

```cpp
std::unique_ptr<cinux::fs::Ext2> ext2(new cinux::fs::Ext2(dev));
auto                             m = ext2->mount();
if (!m.ok()) { /* ... */ return -to_errno(m.error()); }
FileSystem* fs = ext2.release();
if (!vfs_mount_add(target, fs, /*owned=*/true)) { /* ... */ delete fs; return -kEnomem; }
return 0;
```

(`sys_mount.cpp:108-121`,有删节。)`Ext2` 构造函数签名是 `explicit Ext2(cinux::drivers::IBlockDevice* dev)`(`libs/ext2/ext2.hpp:56`)——只吃一个块设备指针,设备必须比 Ext2 实例活得长(注释明说)。`unique_ptr` RAII 守到 `mount()` 成功才 `release()` 交裸指针给挂载表,跟 tmpfs 那条腿是同一个套路。`mount()` 内部读超级块、块组描述符、inode 表——那是 080 章的范围,这里只点到链路存在 + 端到端测试通过。

> **三个 errno 的精确分工,别混。** `ENXIO`(`kEnxio = 6`,`errno.hpp:27`)是 source **解析得通但不是块设备**——`block_device()` 返 `nullptr`,Linux 约定「要求块设备但给了非块设备」正是 `ENXIO`,**不是 `ENODEV`**(`ENODEV` 是「未知 fstype」,语义不同)。`ENOENT`(`kEnoent = 2`,`errno.hpp:23`)是 source 路径**根本不存在**——`vfs_lookup` 返 `NotFound`,工厂经 `to_errno` 映射成 `ENOENT`。`EINVAL`(`kEinval = 22`,`errno.hpp:38`)是 ext2/ext4 没给 source(`source == nullptr || source[0] == '\0'`,在 `:91-93` 第一道门就拦下)。三码互斥,各自定位流水线上不同的故障点——写测试断言时别用错。
>
> 注意 **ext4 走的是同一个分支**。Cinux 没有 `Ext4` 这个类——`strcmp(fstype, "ext4") == 0` 命中的也是 `new Ext2(dev)`。extent-mapped inode(EXT4 的标志特性)靠 inode flag 路由:`inode_has_extent_tree` 检查 `i_flags & EXT4_EXTENTS_FL`(`ext2_extent.hpp:26-28`),命中就走 extent 读路径,否则走传统 indirect block。所以 `sys_mount -t ext4` 在 Cinux 跟 `-t ext2` 是一回事,只是 fstype 字符串不同。extent tree 内部(depth-0 leaf 限制、index node depth>0 返 Unsupported 停读)是 080 章/ext2 卷的范围,本章只点 ext4 复用 Ext2 的**根因**:inode flag 路由,不另起炉灶。

## BlockRegistry + DevFS block node:块设备怎么进 /dev 又怎么被 mount 找到

上一节里 `/dev/sda` 这个名字怎么来的?它不是凭空出现的——boot 期有两个独立的注册点,把同一块物理盘登记进两张表。先看「设备身份」在 boot 期怎么造出来。

**第一步:BlockRegistry——扁平名字表。** 这是 boot 把所有块设备登记进一张 `name → IBlockDevice*` 表的地方:

```cpp
class BlockRegistry {
public:
    static constexpr uint32_t MAX_DEVICES = 16;
    static constexpr uint32_t NAME_MAX   = 32;

    static bool register_device(const char* name, IBlockDevice* dev);
    static IBlockDevice* lookup(const char* name);
    static uint32_t count();
    static const char*   name_at(uint32_t i);
    static IBlockDevice* device_at(uint32_t i);
};
```

(`block_registry.hpp:21-40`。)这是 Linux `major/minor + bdev_map` 的**极简版**——不要 major/minor,只要一个字符串名(`sda`/`sdb`/...)。`MAX_DEVICES = 16`、`NAME_MAX = 32`,够 hobby-scale 用。`register_device`(`block_registry.cpp:36-53`)三道门拒错:null 设备 / **重名** / 表满:

```cpp
bool BlockRegistry::register_device(const char* name, IBlockDevice* dev) {
    if (name == nullptr || dev == nullptr) return false;
    auto g = lock_.guard();
    if (count_ >= MAX_DEVICES) return false;
    for (uint32_t i = 0; i < count_; ++i) {
        if (strcmp(entries_[i].name, name) == 0) {
            return false;  // duplicate name -- a bug in boot wiring
        }
    }
    copy_name(entries_[count_].name, name, NAME_MAX);
    entries_[count_].dev = dev;
    ++count_;
    return true;
}
```

(`block_registry.cpp:36-53`。)注释那句「a bug in boot wiring」点得很直白:重名不是正常情况,是 boot 接线写错了——同名设备不该注册两次,真出现就是 bug。一把 `Spinlock`(`block_registry.cpp:22`)串行化所有访问,SMP-safe。

**第二步:DevFS block node——把 BlockRegistry 的条目投影到 `/dev/` 目录。** 这一步在 `devfs::init` 里:

```cpp
// register /dev/<name> for every block device in the registry.  (有删节——省略源码里的内部 milestone 标记。)
for (uint32_t i = 0; i < cinux::drivers::BlockRegistry::count(); ++i) {
    g_devfs.add_block_node(cinux::drivers::BlockRegistry::name_at(i),
                           cinux::drivers::BlockRegistry::device_at(i));
}
```

(`devfs_init.cpp:168-172`。)这个循环是**桥**——遍历 BlockRegistry,对每条 `add_block_node`,在 DevFS 里造一个 inode。`add_block_node`(`devfs.cpp:323-330`)内部 `new BlockDevOps(dev)` 造一个 ops 实例,指向那块设备,然后 `register_node` 把名字挂进 DevFS 的节点表:

```cpp
void DevFs::add_block_node(const char* name, cinux::drivers::IBlockDevice* dev) {
    if (name == nullptr || dev == nullptr || block_dev_count_ >= MAX_BLOCK_NODES) {
        return;
    }
    auto* ops = new BlockDevOps(dev);
    block_dev_ops_[block_dev_count_++] = ops;
    register_node(name, ops);
}
```

(`devfs.cpp:323-330`。)这样 `/dev/sda` 这个名字**同时被两条路径够得着**:BlockRegistry 名字表(给直接查设备的代码用)+ DevFS 目录项(给走路径解析的 `sys_mount` 用)。两条表通过 `devfs::init` 这个桥保持一致。

boot 注册块设备有**两个生产点**。真硬件 boot 在 `init.cpp`:

```cpp
if (root_bdev != nullptr) {
    cinux::drivers::BlockRegistry::register_device("sda", root_bdev);
}
if (auto* vblk = cinux::drivers::virtio::virtio_block_device()) {
    cinux::drivers::BlockRegistry::register_device(root_bdev ? "sdb" : "sda", vblk);
}
```

(`init.cpp:129-134`。)`root_bdev` 是 NVMe/AHCI 探到的主盘,挂 `sda`;如果还有 virtio_blk 设备,`root_bdev` 已占用 `sda` 就挂 `sdb`,否则它顶上 `sda`——一个简单的冲突决策。测试 boot 在 `main_test.cpp`:

```cpp
if (blk_dev != nullptr) {
    cinux::drivers::BlockRegistry::register_device("sda", blk_dev);
}
```

(`main_test.cpp:230-234`。)测试里只有一块 AHCI port1 的 ext2 盘,直接挂 `sda`。这俩注册点跑完,`devfs::init` 紧跟其后把 BlockRegistry 的条目投影进 `/dev/`——`test_mount_ext2_from_block_device` 那个端到端测就是靠这条链跑通的。

**第三步:BlockDevOps 的三件事。** DevFS 给块设备节点配的 `InodeOps` 子类,override 三样:

```cpp
class BlockDevOps : public InodeOps {
public:
    explicit BlockDevOps(cinux::drivers::IBlockDevice* dev) : dev_(dev) {}

    cinux::lib::ErrorOr<void> stat(const Inode* inode, struct stat* st) override {
        if (inode == nullptr || st == nullptr) return cinux::lib::Error::InvalidArgument;
        memset(st, 0, sizeof(*st));
        st->st_ino     = inode->ino;
        st->st_nlink   = 1;
        st->st_mode    = 0x6000 | 0660;  // S_IFBLK | rw-rw----
        st->st_blksize = 512;
        return {};
    }

    /// Expose the backing block device so sys_mount can resolve this node.
    cinux::drivers::IBlockDevice* block_device(const Inode*) override { return dev_; }

private:
    cinux::drivers::IBlockDevice* dev_;
};
```

(`devfs.cpp:147-168`,有删节——省略了 `// Block device node` 段头注释。)三件事:**(1)** `stat` 给 `S_IFBLK`(0x6000)|0660 + `blksize 512`,告诉用户态「这是个块设备」;**(2)** `block_device` 返 `dev_`,这就是上一节 `sys_mount` 抽 `IBlockDevice` 的钩子;**(3)** 隐式——**不 override `read`/`write`**,文件头注释明说(`devfs.cpp:141-144`):「a block device is consumed by mounting a filesystem over it, not by reading its raw bytes through this inode」。

> **BlockDevOps 不支持 read/write 是有意的边界。** 块设备节点不像普通文件那样存数据,它的存在意义就是被 `sys_mount` **消费**——挂一个 FS 到它上面。对比 Linux 块设备节点也支持裸盘读写(`cat /dev/sda | od` 看裸字节),Cinux **简化掉了**(hobby-scale)。教程里必须如实标这个边界,不能说「能 `cat /dev/sda`」——会失败返 `ENOSYS`(`kEnosys = 38`,`errno.hpp:45`)。证据链闭合:`BlockDevOps` 没 override `read`/`write`,继承 `InodeOps::read`/`write` 的默认实现,默认返 `Error::NotImplemented`(`inode.cpp:15`/`:19`);`to_errno` 把 `NotImplemented` 映射成 `kEnosys`(`errno.hpp:88-89`)。所以块设备节点走 `cat` 就是 `-ENOSYS`,块设备靠 mount fs 消费。BlockRegistry 和 DevFS 是两张表,通过 `devfs::init` 桥接;若有人绕过 `devfs::init` 直接改其中一张(比如往 BlockRegistry 加设备但没触发 DevFS 重投影),两张表会不一致——这是个耦合点,boot 接线纪律靠 `devfs::init` 紧跟在 `BlockRegistry::register_device` 之后来保证。

## InodeOps::block_device():一个「设备身份」槽

上一节用到的 `block_device()` 这个虚方法,值得单独拎出来讲透——它是整条链能把「路径」转成「设备对象」的**唯一钩子**。先看基类默认实现:

```cpp
/// backing block device for a block-device inode (e.g. /dev/sda in DevFs).
/// sys_mount resolves its source path to an Inode, then calls this to obtain
/// the IBlockDevice to mount.  Default nullptr -- regular files, char devices
/// and pseudo-fs nodes have no block-device backing.  (有删节——省略源码里的内部 milestone 标记。)
virtual cinux::drivers::IBlockDevice* block_device(const Inode* /*inode*/) {
    return nullptr;
}
```

(`inode.hpp:230-236`。)`InodeOps` 是所有 inode ops 的基类——`Ext2FileOps`、`TmpFileOps`、`TmpDirOps`、`ProcStatFileOps`、`DevDirOps`、`BlockDevOps`...全都是它的子类。这个虚方法**默认返 `nullptr`**,只有 `BlockDevOps` override 返 `dev_`(`devfs.cpp:163-164`)。要核「全树多少处 override」,得把 grep 收紧到只匹配代码行,不然文档注释里同含 `block_device` 和 `override` 两个词的那行(`devfs.hpp:151` 的注释)也会被命中——用 `grep -rn 'block_device.*override' kernel/ libs/ | grep -v '///'` 过滤掉文档注释,应见唯一一处真 override:`devfs.cpp:164`。

这个设计的精妙在于「**多态默认值**」:现有所有 `InodeOps` 子类(几十个)vtable 多一个槽,但**行为零变化**——它们继承默认 `nullptr`,不需要写任何代码。`sys_mount` 拿到 `nullptr` 就判 `ENXIO`。好处很明显:**`sys_mount` 不需要知道 DevFS 内部结构、不 down-cast、不硬编码「如果是 DevFs 就走某条路」**,全靠虚分派——加一个新的 FS 子类(比如将来加个 nfs),它的 inode ops 自然继承 `nullptr`,不需要动 `sys_mount` 一行。代价是 `InodeOps` vtable 每类多一个指针槽(几十个虚类 × 一个指针,几百字节,可忽略)。

这是「**给基类加虚方法却不破坏派生类**」的标准手法:默认实现承担绝大多数派生类的正确行为(无块设备),只有真正有块设备的子类 override。对比 Linux 的 `block_device` 是独立的 `struct file_operations`——Cinux 选择复用 `InodeOps` 多态,代价是 `block_device()` 进了所有 inode 的 vtable;好处是 `sys_mount` 一行虚调用就拿到设备,不用类型开关。

> **为什么必须 `NoFollow`?** 看起来多余——`/dev/sda` 谁会做成 symlink?但语义上必要:`Follow` 模式下若末段是 symlink,会被 follow 到目标,目标可能不再是块设备节点,`block_device()` 返 `nullptr` 误判 `ENXIO`。`NoFollow` 保证拿到的是路径末段那个 inode **本身**,不是它指向的东西。和 `readlink`/`lstat` 用 `NoFollow` 同一原理:要看节点本身,不看它指向哪。这条纪律在 `sys_mount.cpp:88` 的注释里明写:「`NoFollow: /dev/sda is the device node itself, not a symlink to follow`」。

## 挂载表 MountPoint + owned 生命周期(承 081)

owned 这个 bool 的故事,081 tmpfs 章讲过——那里看的是 `owned=true` 在 tmpfs 通路怎么用(堆 `new TmpFs` → `release()` → `vfs_mount_add(owned=true)` → `umount2` 走 `free_tree` 回收整棵树)。这里看它在挂载表**全貌怎么收敛三类对象**。先看结构:

```cpp
struct MountPoint {
    char        path[MOUNT_PATH_MAX];  ///< Absolute path prefix (e.g. "/")
    FileSystem* fs;                    ///< Concrete filesystem backend
    bool        in_use;                ///< Whether this slot is occupied
    /// True when @p fs was heap-allocated by sys_mount and must therefore be
    /// `delete`d by vfs_mount_remove (sys_umount2).  Boot/static mounts wire a
    /// static FileSystem (g_devfs, g_procfs, g_tmpfs, ...) and leave this false,
    /// so removing them frees nothing.  Default false keeps every existing
    /// caller (boot wiring + tests with stack-local/mock backends) unchanged.
    bool        owned{false};
};
```

(`vfs_mount.hpp:38-48`。)挂载表是定长数组 `MountPoint g_mount_table[MOUNT_TABLE_SIZE]`(`MOUNT_TABLE_SIZE = 8`,`vfs_mount.hpp:26`),一把 `Spinlock` 串行化(`vfs_mount.cpp:26`)。每个槽四个字段:路径前缀、FS 指针、是否占用、**是否 owned**。

三类对象在一个 bool 上收敛:

**第一类:boot/static 接线,`owned=false`。** boot 时挂的根 FS(`/`，`init.cpp:123` 的 `vfs_mount_add("/", rootfs)`)、tmpfs(`/tmp`，`tmpfs_init.cpp:44`)、devfs(`/dev`，`devfs_init.cpp:174`)、procfs(`/proc`，`procfs_init.cpp:39`)——这四个对象是 `.bss` 静态全局或 boot 栈局部,命超表(整个内核运行期)。`vfs_mount_add` 默认 2-arg(`owned = false`,`vfs_mount.hpp:79`),挂载表只存指针、不管释放。`umount2` 看到这类只摘槽、不 delete。

**第二类:`sys_mount` 堆分配,`owned=true`。** tmpfs 分支(`sys_mount.cpp:61`)和 ext2/ext4 分支(`sys_mount.cpp:115`)——`new` 一个后端 FS,`mount()` 成功后 `release()` 交裸指针给挂载表,`owned=true`。`umount2` 看到 owned 就 `delete fs`,tmpfs 的 `delete` 触发 `free_tree` 递归回收整棵树,ext2 的 `delete` 触发 `~Ext2` 释放缓存对象。

**第三类:`sys_mount -t proc/devfs` 二次挂载单例,`owned=false`。** `sys_mount.cpp:79` 那条腿——用户运行时 `mount -t proc none /mnt/proc2`,挂载表登记一个新槽指向 boot 单例,`owned=false`。`umount2 /mnt/proc2` 只摘新挂点,**原 `/proc` 不动**——单例的命跟 boot 接线一样长,二次挂载只是给它多一个路径前缀。

这三类的回收分叉全在 `vfs_mount_remove` 里:

```cpp
bool vfs_mount_remove(const char* path) {
    if (path == nullptr) return false;
    auto g = g_mount_lock.guard();
    for (uint32_t i = 0; i < MOUNT_TABLE_SIZE; ++i) {
        if (g_mount_table[i].in_use && strncmp(g_mount_table[i].path, path, MOUNT_PATH_MAX) == 0) {
            // Ownership-aware teardown: a sys_mount-created backend (owned=true)
            // is heap-allocated and has no other owner, so delete it here.  A
            // boot/static mount (owned=false) wires a static object and is left
            // alone -- its lifetime exceeds the table.  The TmpFs / ProcFs / DevFs
            // destructors never touch g_mount_lock, so this is safe under it.
            if (g_mount_table[i].owned) {
                delete g_mount_table[i].fs;
            }
            g_mount_table[i].in_use = false;
            g_mount_table[i].fs     = nullptr;
            g_mount_table[i].owned  = false;
            return true;
        }
    }
    return false;
}
```

(`vfs_mount.cpp:79-104`。)一个 `if (owned) delete fs` 就是全部分叉——`owned=true` 先 `delete fs` 再清槽,`owned=false` 只摘点。这一节短,核心就是「081 讲过的 owned,这里看它在挂载表全貌收敛三类」——tmpfs 通路的 RAII 细节(`unique_ptr` 守到 `mount()` 成功才 `release`、错误腿显式 `delete`)回 081 看,这里不重讲。

> **新手会本能想给 `FileSystem` 加 `virtual Destroy()` / `shared_ptr` / deleter / `MountKind` 枚举。** Cinux 的解法极朴素:结构体里多一个 bool 默认 false。boot 接线和 `sys_mount` 走**同一个 `vfs_mount_add`**,差别只在第三个实参。这是「用默认参兼容旧调用者」的典型范例——加字段不破坏任何既有 2-arg 调用(所有 boot 接线 + 测试栈/mock FS 都不用改一行)。一个坑:若误把 boot 静态挂载标 `owned=true`,`umount` 时 `delete` 静态对象 → 双重释放/崩溃。`test_remount_after_umount_is_fresh`(`test_mount.cpp:104-125`)是 owned 语义最硬的证据——`umount` 后再 `mount` 同路径,文件不见了,证明 owned 后端真被 `delete` 了,新 `mount` 是全新实例不是 stale 残留。

## umount2:摘槽 vs delete 的分叉

`do_umount2_kernel` 是 `do_mount_kernel` 的镜像——挂的反面就是摘。代码很短:

```cpp
int64_t do_umount2_kernel(const char* target, [[maybe_unused]] uint64_t flags) {
    // MNT_FORCE / MNT_DETACH / MNT_EXPIRE not yet modelled

    if (target == nullptr || target[0] == '\0') {
        return -kEinval;
    }
    if (!cinux::fs::vfs_mount_remove(target)) {
        return -kEnoent;  // nothing mounted at that path
    }
    return 0;
}
```

(`sys_umount2.cpp:22-32`。)三件事:**(1)** flags 标 `[[maybe_unused]]`,意味着这个参数在函数体里**根本没被读**——纯 ABI 占位;**(2)** target 空/null 返 `-kEinval`;**(3)** `vfs_mount_remove` 返 false(路径不在挂载表里)返 `-kEnoent`。至于「摘槽 vs delete」的分叉,全在上一节贴过的 `vfs_mount_remove` 里——`owned=true` 的(tmpfs/ext2)连 FS 实例一起 delete,`owned=false` 的(boot 静态/proc/devfs 二次挂)只摘点。

上层 `sys_umount2`(`sys_umount2.cpp:34-40`)只做 `resolve_user_path(target)` 一层 SMAP 读取,然后委托 `do_umount2_kernel`:

```cpp
int64_t sys_umount2(uint64_t target_virt, uint64_t flags, uint64_t, uint64_t, uint64_t, uint64_t) {
    cinux::fs::PathBuf target;
    if (!resolve_user_path(target_virt, target.data())) {
        return -kEfault;
    }
    return do_umount2_kernel(target.data(), flags);
}
```

(`sys_umount2.cpp:34-40`。)`resolve_user_path` 是 SMAP 安全读取(`path_util.hpp:65`)——把用户态地址里的路径字符串拷到内核栈,再交给 `do_umount2_kernel` 处理。

> **umount2 的 `MNT_FORCE` 是纯 ABI 占位,别误以为有强制卸载。** grep 全树(`kernel/` 目录)没有任何 `#define MNT_FORCE`/`MNT_DETACH`/`MNT_EXPIRE` 的命中——这几个名字只出现在 `sys_umount2.cpp:23` 的注释里(`// MNT_FORCE / MNT_DETACH / MNT_EXPIRE not yet modelled`)和 `.hpp` 文件头说明里,运行期连 `if (flags & MNT_FORCE)` 这种判断都没有,只走 `vfs_mount_remove(target)` 一条路。这是最容易 overclaim 的地方——本章不能宣称「支持强制卸载」,只能写「接受 flags 为 ABI 兼容,行为等同 `umount(target)`」。`test_mount.cpp` 全程传 `flags=0`(`test_mount.cpp:59`/`:84`/`:96` 等),也没覆盖非零 flags 路径——因为根本没有非零 flags 路径。

## 验证

三层验证,合起来才够。

**第一层:host 单测——挂载表纯逻辑有,工厂逻辑没有。** 挂载表本身(`vfs_mount_add`/`remove`/`resolve`/最长前缀匹配)是纯逻辑,有 host 单测 `test/unit/test_vfs_mount.cpp`(19 个 TEST,注册在 `test/CMakeLists.txt:255`,链真 `vfs_mount.cpp`,`MockFileSystem` 只是后端替身)。这层覆盖:空表、加单点、加多点、表满返 false、nullptr path/fs 返 false、空路径返 false、正常删除、删不存在返 false、精确匹配、最长前缀匹配(`/mnt` 赢过 `/`)、无匹配返 nullptr、remove+re-add 循环——挂载表的表操作全测到了。但 **factory 四类分支(tmpfs/proc/devfs/ext2)依赖内核态的 `TmpFs`/`ProcFs`/`DevFs`/`Ext2` + DevFS + BlockRegistry,host 上没有这些类**——工厂逻辑验证全在内核态。

**第二层:kernel 测试 `test_mount.cpp`,8 例覆盖 factory 全四类。** `run_mount_tests`(`test_mount.cpp:184-195`)注册 8 例,进 `big_kernel_test`(节头字面量保留源码原样,里头的内部 milestone 标记读者不用关心):

```
RUN_TEST(test_mount::test_mount_tmpfs_resolves);           // tmpfs 基本链
RUN_TEST(test_mount::test_mount_then_create_write_read);   // tmpfs 全闭环
RUN_TEST(test_mount::test_mount_unknown_fstype_is_enodev); // 错误码
RUN_TEST(test_mount::test_umount_missing_is_enoent);       // umount 错误码
RUN_TEST(test_mount::test_remount_after_umount_is_fresh);  // ownership 回收
RUN_TEST(test_mount::test_mount_proc_shares_singleton);    // proc 单例共享
RUN_TEST(test_mount::test_mount_devfs_factory);            // devfs 单例/未 init
RUN_TEST(test_mount::test_mount_ext2_from_block_device);   // /dev/sda → Ext2 端到端
```

(`test_mount.cpp:184-195`。)分类看:**(a)** tmpfs 基本链 `test_mount_tmpfs_resolves`(`:56-61`,mount → resolve → umount → detach)+ `test_mount_then_create_write_read`(`:63-85`,挂 → create/write/read 全闭环,字节对得上);**(b)** 错误码 `test_mount_unknown_fstype_is_enodev`(`:87-93`,ext2 无 source → `EINVAL` / nonsense fstype → `ENODEV`)+ `test_umount_missing_is_enoent`(`:95-99`,不存在 → `ENOENT`,空/null → `EINVAL`);**(c)** ownership `test_remount_after_umount_is_fresh`(`:104-125`,owned 后端真释放,二次 mount 文件不见);**(d)** 单例 `test_mount_proc_shares_singleton`(`:131-143`,二次挂 /mnt/proc2 共享 boot 单例,umount 后原 /proc 还在)+ `test_mount_devfs_factory`(`:149-162`,boot init 过就共享单例、没 init 过 factory 返 `ENODEV`);**(e)** 块设备 `test_mount_ext2_from_block_device`(`:168-180`,`/dev/sda` → Ext2 真挂,root inode 解析得通)。

> **`test_mount_ext2_from_block_device` 的 guard 不是 skip。** 它开头有一句:
>
> ```cpp
> if (cinux::drivers::BlockRegistry::lookup("sda") == nullptr) {
>     return;  // no block device registered (slim boot without a disk) -- skip
> }
> ```
>
> (`test_mount.cpp:169-171`。)看起来像「skip」,但 `run-kernel-test` 的 boot 路径 `main_test.cpp:230-234` 已经注册了 `sda`(AHCI port1 ext2 盘),guard **永不触发**——8 个断言全跑真路径。注释里的「skip」是给 slim boot(没盘的极简启动)留的退路,正常跑测试套件时 `BlockRegistry::lookup("sda")` 必非空。`RUN_TEST` 宏(`big_kernel_test.h:174-183`)靠 `_failed_before` 对比计 pass——guard 里的 `return` 不增加 `tests_failed`,所以这例就算 guard 触发也是「静默通过」不是 fail,但正常 boot 它跑的是真断言。

**第三层:`make run` 冒烟 + busybox mount/umount。** 起真内核(`cmake --build build --target run`),看 boot 序列:

- `[VFS] ext2 mounted at /`(`init.cpp:124`)——根 FS 挂上
- `[DEVFS] mounted at /dev (...)`(`devfs_init.cpp:178`)——DevFS 挂上,括号里是节点数
- `[PROCFS] mounted at /proc`(`procfs_init.cpp:43`)——ProcFS 挂上
- `[TMPFS] mounted at /tmp`(`tmpfs_init.cpp:48`)——tmpfs 挂上(081 章验过)

进 shell 后用 busybox 做一组 mount/umount 冒烟(具体命令在 lab 里):`mount -t tmpfs none /mnt/tmp` 真挂一个、`touch` 文件、`umount /mnt/tmp`、再 `mount` 同路径确认文件没了(owned 真回收)。这是用户态可见的端到端证据。

> **测试数字怎么填。** `run-kernel-test-all` 两腿(单核 + `-smp 2`)的通过数,得在 Book 工作树真跑后填,**不照抄源仓库 dev note 的数**——那是源仓库的,Book 侧须独立验证。`scripts/check_test_count.sh` 就是干这个的,基线 `CINUX_TEST_BASELINE` 默认 875,mount 这 8 例都进 `big_kernel_test`,真跑后 passed 数应 ≥ 基线 + 这些例。用户态真能用 mount/umount 靠 boot 冒烟日志(`[VFS] ext2 mounted at /` 等) + busybox smoke 两腿绿间接证明,不是 `big_kernel_test` 的直接断言——三层证据合力,任一单独都不够。
>
> **ring0 测直驱 `do_mount_kernel` 而非 `sys_mount`。** 注意 `test_mount.cpp` 全程调 `do_mount_kernel`/`do_umount2_kernel`,不调 `sys_mount`/`sys_umount2`——为什么?`sys_` 包装层(`sys_mount.cpp:131-157` / `sys_umount2.cpp:34-40`)第一件事就是 SMAP user-ptr 读取(`resolve_user_path`/`read_user_path`),内部经 `is_user_vaddr` 判定只接受**用户态地址**。`run-kernel-test` 机制测跑在 ring0 内核栈,传内核地址 `is_user_vaddr` 直接拒 → `-kEfault`。所以测必须绕过 user-ptr 层直驱 `do_` 内核变体——它们才是真正的工厂逻辑入口。这是「syscall handler 的 user/kernel 边界 vs 核心逻辑」分层的好例子:`do_` 是纯内核 API(可测可复用),`sys_` 只是 `do_` 外面套一层 user-ptr 安全读取。

## 这章没做的

- **`MS_*` flags accepted but ignored。** `sys_mount.cpp:46` 注释明写「MS_* flags are accepted for Linux ABI parity but not yet modelled」——`MS_RDONLY`/`MS_NOEXEC`/`MS_NOSUID`/`MS_REMOUNT` 这些全收下不解析,挂出来都是可读写可执行。一个能 mount 的内核,第一步是「接受这个调用不让它崩」,第二步才是「真的按 flag 行事」——Cinux 现在在第一步。
- **`MNT_FORCE`/`MNT_DETACH`/`MNT_EXPIRE` 同性质。** `sys_umount2.cpp:23` 注释明标 not yet modelled,无强制卸载/懒卸载。`flags` 参数标 `[[maybe_unused]]`,运行期根本没有非零 flags 路径。
- **没有 `/proc/mounts`。** 用户态没法查挂载表——ProcFS 动态节点扩展没做,busybox `mount`(无参)列不出当前挂载。但挂载本身是生效的(`vfs_resolve` 能命中),用 `ls /mnt/...` 之类间接验。
- **忙挂载 `umount` 不返 `EBUSY` 直接回收。** 挂载表 `MountPoint` 有 `in_use` 字段(占槽标记),但**没有引用计数门**——`umount` 不查「有没有 open fd 指向这棵树」,直接 `vfs_mount_remove` 回收。Linux 是「有 fd 持有就 `EBUSY`」,Cinux 简化掉了,留 follow-up。
- **没有 bind mount / mount namespace / mount propagation。** 这些 Linux 高级特性 Cinux 未建模,挂载表是单一全局表(`g_mount_table`),没有 per-namespace 隔离。
- **mount options 字符串(`data` 参数)未解析。** `sys_mount.cpp:133` 注释「mount options string -- not yet parsed」——`sys_mount` 的第六个参数 `data` 标 `[[maybe_unused]]`,运行期不读。`mount -o size=...` 之类的选项字符串收下但丢弃。
- **DevFS `BlockDevOps` 不支持 read/write。** `devfs.cpp:141-144` 注释明说,块设备靠 mount fs 消费,用户态不能 `cat /dev/sda` 看裸字节。对比 Linux 块设备节点也支持裸盘读写,Cinux 简化掉了。
- **host `test_vfs_mount` 历史债。** 本章核实了它在 `test/CMakeLists.txt:255` 注册、链真 `vfs_mount.cpp`、19 个 TEST 覆盖表操作。但教程引用它作「factory 证据」前建议实跑一次 `ctest` 核实——它只测挂载表纯逻辑,**不覆盖 factory 四类分支**(那依赖内核态类,host 没有)。

## 小结

- **mount factory 是 fstype 驱动的工厂**——`do_mount_kernel` 按 `fstype` 字符串走路由,四类分支对应四种 FS 与后端关系:tmpfs 堆 new、proc/devfs 取 boot 单例、ext2/ext4 走块设备链、未知返 `ENODEV`。`sys_mount` 只看 fstype 字符串,不关心后端怎么造。
- **块设备挂载链是三层解析**——source 字符串 → `vfs_lookup(NoFollow)` 拿 `Inode` → `block_device()` 虚方法抽 `IBlockDevice` → `new Ext2(dev)`。`InodeOps::block_device()` 是「设备身份」槽,默认 `nullptr`,只有 DevFS 的 `BlockDevOps` override 返 `dev_`——这是「加虚方法却不破坏派生类」的标准范例。
- **`owned` bool 统一三类生命周期**(承 081)——堆 `owned=true` / 单例 `owned=false` / 未知 `ENODEV`,在 `MountPoint.owned` 一个 bool 上收敛,默认 false 兼容所有旧 2-arg 调用。`vfs_mount_remove` 一个 `if (owned) delete fs` 就是全部分叉。
- **四个 errno 精确分工**——`EINVAL`(参数缺)/ `ENOENT`(source 路径不存在)/ `ENXIO`(source 解析得通但不是块设备)/ `ENODEV`(未知 fstype 或单例未 init)。写测试断言别用错。
- **诚实边界**——`MS_*`/`MNT_*` flags accepted but ignored、无 `/proc/mounts`、忙挂载不返 `EBUSY`、`BlockDevOps` 不支持裸读裸写、mount options 字符串未解析、无 bind mount / namespace / propagation。这些是工程折中,不是漏——一个能 mount 的内核,第一步是「接受调用不让它崩」,真正的 flag 建模留到后续。

至此 VFS 的「挂载侧」全貌齐了:081 讲了 tmpfs 那一行怎么填进表,本章讲了表的填充侧全四类 + 块设备链;079 讲的是表的消费侧(`vfs_resolve` 跨挂载点 + flock + dentry cache)。三章合起来,VFS 的 finale 才算完整——`sys_mount` 接进来,`vfs_resolve` 走出去,中间那张挂载表就是它俩的握手协议。
