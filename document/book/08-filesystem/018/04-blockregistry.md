---
title: 04 · BlockRegistry + DevFS block node
---

# BlockRegistry + DevFS block node

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
