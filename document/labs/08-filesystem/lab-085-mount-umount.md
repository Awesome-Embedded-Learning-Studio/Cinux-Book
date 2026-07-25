---
title: Lab 085 · mount/umount 验证
---

# Lab 085 · mount/umount 验证

> 对应 `document/book/08-filesystem/085-mount-umount.md`。验证档 **A 档**:这一章交付的是 `sys_mount -t ext2 /dev/sda` 真能挂一块盘、`umount2` 能干净回收、mount factory 四类分支各走各的路。注意挂载表纯逻辑有 host 单测,**factory 四类分支没有**——依赖内核态类(`TmpFs`/`ProcFs`/`DevFs`/`Ext2` + DevFS + BlockRegistry),host 上没有,工厂逻辑验证全在 kernel harness;真挂一块 ext2 盘 + busybox smoke 靠 `make run` 冒烟。

## 目标

确认五件事:

1. **mount factory 是 fstype 驱动的四类分发**——`do_mount_kernel` 按 `fstype` 字符串走路由(tmpfs / proc·devfs / ext2·ext4 / 未知 `ENODEV`),跟 081 tmpfs 章是同一张表的不同行;
2. **块设备挂载链是三层解析**(079 deferred 的核心)——source 路径 → `vfs_lookup(NoFollow)` 拿 `Inode` → `block_device()` 虚方法抽 `IBlockDevice` → `new Ext2(dev)`;
3. **`owned` bool 统一三类生命周期**(承 081)——堆 owned / 单例 unowned / 未知 ENODEV;
4. **四个 errno 精确分工**——`EINVAL`/`ENOENT`/`ENXIO`/`ENODEV`;
5. **boot 真挂 ext2 根盘**:`make run` 见 `[VFS] ext2 mounted at /`,busybox 能在运行时 mount/umount tmpfs(owned 真回收)。

## 步骤

### 1. host 单测:挂载表纯逻辑

挂载表的表操作(init/add/remove/resolve/最长前缀匹配)有 host 单测,先跑这个:

```bash
cmake --build build --target test_vfs_mount 2>&1 | tail
ctest --test-dir build -R vfs_mount 2>&1 | tail
```

应见 `test/unit/test_vfs_mount.cpp` 的 19 个 TEST 全 PASS(注册在 `test/CMakeLists.txt:255`,链真 `kernel/fs/vfs_mount.cpp`,`MockFileSystem` 只是后端替身)。覆盖:空表、加单点、加多点、表满返 false、nullptr path/fs 返 false、空路径返 false、正常删除、删不存在返 false、精确匹配、**最长前缀匹配(`/mnt` 赢过 `/`)**、无匹配返 nullptr、remove+re-add 循环。

> 这层**不覆盖 factory 四类分支**——`do_mount_kernel` 的 tmpfs/proc/devfs/ext2 分支依赖内核态类(`TmpFs`/`ProcFs`/`DevFs`/`Ext2` + DevFS + BlockRegistry),host 上没有。factory 逻辑验证走 kernel harness 下一节。

### 2. kernel 测试:test_mount 八例全跑

factory + umount 语义 + 错误码 + ownership,全在 kernel 测试里:

```bash
cmake --build build --target run-kernel-test 2>&1 | grep -A2 "mount/umount2"
```

应见 `run_mount_tests`(`test_mount.cpp:184-195`)注册的 8 例 PASS,挂在一个 mount/umount2 section 头下(`test_mount.cpp:185` 的 `TEST_SECTION`):

| 例 | 测什么 | 行号 |
|----|--------|------|
| `test_mount_tmpfs_resolves` | tmpfs 基本链 mount→resolve→umount→detach | `:56-61` |
| `test_mount_then_create_write_read` | tmpfs 全闭环 create/write/read 字节对得上 | `:63-85` |
| `test_mount_unknown_fstype_is_enodev` | ext2 无 source→`EINVAL`、nonsense→`ENODEV` | `:87-93` |
| `test_umount_missing_is_enoent` | 不存在→`ENOENT`、空/null→`EINVAL` | `:95-99` |
| `test_remount_after_umount_is_fresh` | owned 后端真释放,二次 mount 文件不见 | `:104-125` |
| `test_mount_proc_shares_singleton` | proc 二次挂共享 boot 单例,umount 后原 /proc 还在 | `:131-143` |
| `test_mount_devfs_factory` | devfs init 过共享单例、没 init 返 `ENODEV` | `:149-162` |
| `test_mount_ext2_from_block_device` | `/dev/sda`→Ext2 真挂,root inode 解析得通 | `:168-180` |

> **`test_mount_ext2_from_block_device` 的 guard 不是 skip。** 它开头 `if (BlockRegistry::lookup("sda") == nullptr) return;`(`test_mount.cpp:169-171`)看起来像 skip,但 `run-kernel-test` 的 boot 路径 `main_test.cpp:230-234` 已经注册了 `sda`(AHCI port1 ext2 盘),guard **永不触发**——8 个断言全跑真路径。grep boot 日志确认:

```bash
cmake --build build --target run-kernel-test 2>&1 | grep -iE "sda|ext2 mounted|BlockRegistry"
```

应见 `ext2 mounted at / for smoke` 这类行(`main_test.cpp:228`,行里带有源码内部 milestone 标记,读者不用关心那串代号,认 `ext2 mounted at /` 这个子串即可)——`sda` 真注册了,guard 不触发。

### 3. factory 四类分发:数 strcmp 出现的次数

`do_mount_kernel` 是个按 `fstype` 字符串分派的工厂。grep 一下到底分了几类:

```bash
grep -n 'strcmp(fstype,' kernel/syscall/sys_mount.cpp
```

应见 **5 行命中**,但只有 4 行在工厂主体 `do_mount_kernel` 里,第 5 行在 wrapper 里。逐行对照:

- `:52` `tmpfs` —— 工厂第一分支。
- `:72` `proc` / `devfs`(同一 `if` 用 `||` 连)—— 工厂第二分支。
- `:73` `proc`(三目运算符里再判一次,用来在 `procfs::instance()` 和 `devfs::instance()` 之间二选一)—— 工厂第二分支内部的选择,不算第四类。
- `:90` `ext2` / `ext4`(同一 `if` 用 `||` 连)—— 工厂第三分支。
- `:149` `ext2` / `ext4`(再判一次)—— **不在工厂里**,在 `sys_mount` wrapper(`:131-157`)的 source-read guard,只对 ext2/ext4 才读 `source` 用户态路径。

所以「四类 fstype 分发」对应工厂主体里的 4 行(`:52`/`:72`/`:73`/`:90`),兜底分支在 `:123-128` 没 strcmp 直接 `return -kEnodev`,wrapper 的 `:149` 那行是 ext2/ext4 专属的 user-ptr 读取门,别把它误数进工厂分支。读者跑 grep 看到 5 行属正常,5 行各自归属见上。

> **ext4 复用 Ext2**。`strcmp(fstype, "ext4") == 0` 命中的也是 `new Ext2(dev)`(`sys_mount.cpp:108`)——Cinux 没有 `Ext4` 类,extent-mapped inode 靠 `i_flags & EXT4_EXTENTS_FL`(`ext2_extent.hpp:26-28`)路由,不另起炉灶。所以 `-t ext4` 和 `-t ext2` 在 Cinux 是一回事。

### 4. 块设备挂载链:三层解析逐行核

本章核心。完整贴出来逐层核:

```bash
sed -n '90,121p' kernel/syscall/sys_mount.cpp
```

三层一眼能看到:

- **第一层 `vfs_lookup(source, NoFollow)`**(`:94-95`)——`LookupFlag::NoFollow = 1u << 5`(`vfs_lookup.hpp:39`),语义「末段是 symlink 也不要 follow」。`/dev/sda` 是设备节点本身不是软链,但语义上必须 NoFollow——否则 Follow 模式下若末段偶为 symlink 会被解析到非块设备目标误判 `ENXIO`。
- **第二层 `ino->ops->block_device(ino)`**(`:100-101`)——`InodeOps` 虚方法,默认 `nullptr`(`inode.hpp:230-236`),只有 DevFS 的 `BlockDevOps` override 返 `dev_`(`devfs.cpp:163-164`)。拿到 ref 后必须 `inode_unref(ino)`(`:102-104`)平账,否则引用计数泄漏。`dev == nullptr` 返 `-kEnxio`(`:105-107`)。
- **第三层 `new Ext2(dev)` + `mount()`**(`:108-113`)——`unique_ptr` RAII 守到成功才 `release()`(`:114`),`vfs_mount_add(target, fs, /*owned=*/true)`(`:115`)。`Ext2` 构造签名 `explicit Ext2(IBlockDevice* dev)`(`libs/ext2/ext2.hpp:56`)。

### 5. 三个 errno 的精确分工

最容易混的三个 errno,写测试断言时别用错。读 `test_mount.cpp:87-99` 对照:

```bash
sed -n '87,99p' kernel/test/test_mount.cpp
```

应见:

- **`EINVAL`**(`kEinval = 22`,`errno.hpp:38`)——ext2/ext4 没给 source(`source == nullptr || source[0] == '\0'`,`sys_mount.cpp:91-93`)。`test_mount_unknown_fstype_is_enodev`(`:89`)第一句 `do_mount_kernel(nullptr, kPathA, "ext2", 0)` 断言返 `-kEinval`。
- **`ENODEV`**(`kEnodev = 19`,`errno.hpp:35`)——未知 fstype(`sys_mount.cpp:127-128`)或 proc/devfs 单例未 init(`sys_mount.cpp:77`)。`test_mount_unknown_fstype_is_enodev`(`:91`)第二句 `do_mount_kernel(nullptr, kPathA, "nonsense", 0)` 断言返 `-kEnodev`。
- **`ENOENT`**(`kEnoent = 2`,`errno.hpp:23`)——source 路径不存在(`vfs_lookup` 返 `NotFound`,经 `to_errno` 映射)或 umount target 不在挂载表(`sys_umount2.cpp:29`)。`test_umount_missing_is_enoent`(`:96`)断言 `-kEnoent`。
- **`ENXIO`**(`kEnxio = 6`,`errno.hpp:27`)——source 解析得通但不是块设备(`block_device()` 返 `nullptr`,`sys_mount.cpp:106`)。Linux 约定「要求块设备但给了非块设备」正是 `ENXIO`,**不是 `ENODEV`**——这俩 errno 语义不同,别混。

### 6. owned 生命周期:三类在一个 bool 上收敛

挂载表的 `owned` 字段(`vfs_mount.hpp:47`)收敛三类对象。逐个核:

```bash
sed -n '38,48p' kernel/fs/vfs_mount.hpp   # MountPoint 结构 + owned 注释
sed -n '79,104p' kernel/fs/vfs_mount.cpp  # vfs_mount_remove 的 if(owned) delete 分叉
```

`vfs_mount_remove` 的核心就是 `if (g_mount_table[i].owned) delete g_mount_table[i].fs;`(`vfs_mount.cpp:93-95`)——owned=true 连 FS 一起 delete,owned=false 只摘点。三类对照:

- **boot/static 接线 owned=false**——`init.cpp:123`(`/`)、`tmpfs_init.cpp:44`(`/tmp`)、`devfs_init.cpp:174`(`/dev`)、`procfs_init.cpp:39`(`/proc`)全用默认 2-arg;
- **sys_mount 堆分配 owned=true**——`sys_mount.cpp:61`(tmpfs)、`sys_mount.cpp:115`(ext2/ext4);
- **sys_mount 二次挂单例 owned=false**——`sys_mount.cpp:79`(proc/devfs 二次挂共享 boot 单例)。

最硬的证据是 `test_remount_after_umount_is_fresh`(`test_mount.cpp:104-125`):

```bash
sed -n '104,125p' kernel/test/test_mount.cpp
```

应见:mount tmpfs → create 一个 `stale` 文件 → umount(owned backend 真 delete,free_tree 回收整棵树)→ 再 mount 同路径 → `lookup_or_null(fs, "stale")` 断言 `nullptr`(`:122`)。**文件不见了 = owned 后端真被 delete 了,新 mount 是全新实例不是 stale 残留**。

> **boot 误标 owned=true 会炸。** 若 boot 静态挂载误传 `owned=true`,`sys_umount2("/tmp")` 会在 `vfs_mount_remove` 里 `delete` 静态 `g_tmpfs` —— 双重释放/崩溃。所以 `tmpfs::init`、`devfs::init`、`procfs::init` 全用默认 2-arg(`owned=false`)是**刻意的安全约束,不是省事**。这条纪律写在 `vfs_mount.hpp:42-47` 的注释里。

### 7. BlockRegistry + DevFS block node:块设备怎么进 /dev

块设备身份在 boot 期登记进两张表,通过 `devfs::init` 桥接。逐个核:

```bash
# BlockRegistry:扁平名字表(无 major/minor,只要字符串名)
sed -n '21,40p' kernel/drivers/block_registry.hpp
sed -n '36,53p' kernel/drivers/block_registry.cpp   # register_device 三道门:null/重名/表满
```

应见 `MAX_DEVICES = 16` / `NAME_MAX = 32`(`block_registry.hpp:23-24`),`register_device` 注释「duplicate name -- a bug in boot wiring」(`block_registry.cpp:46`)——重名不是正常情况,是 boot 接线写错了。

```bash
# devfs::init 桥:遍历 BlockRegistry 投影进 /dev/<name>
sed -n '168,172p' kernel/fs/devfs/devfs_init.cpp
```

应见 `for (i in BlockRegistry::count()) g_devfs.add_block_node(name_at(i), device_at(i));`——这是 BlockRegistry 和 DevFS 两张表的桥。

```bash
# BlockDevOps override 三件事
sed -n '147,168p' kernel/fs/devfs/devfs.cpp
```

应见 `stat` 给 `S_IFBLK`(0x6000)|0660 + `blksize 512`(`:158-159`)、`block_device` 返 `dev_`(`:163-164`)、**没 override read/write**(`:141-144` 注释明说块设备靠 mount fs 消费不直接读裸字节)。没 override 意味着继承 `InodeOps::read`/`write` 默认实现(`inode.cpp:15`/`:19` 返 `NotImplemented`),`to_errno` 把它映成 `-ENOSYS`(`errno.hpp:88-89`,`kEnosys = 38`)——所以 `cat /dev/sda` 失败返的就是 `ENOSYS`,不是别的 errno。

两个 boot 注册点对照:

```bash
grep -n "register_device" kernel/proc/init.cpp kernel/test/main_test.cpp
```

应见生产 boot `init.cpp:130`(sda=root_bdev)+ `:133`(sdb 或 sda=virtio_blk,看冲突);测试 boot `main_test.cpp:233`(sda=AHCI port1 ext2 盘)。

### 8. InodeOps::block_device() 虚方法:设备身份槽

整条链最反直觉的设计——把 `IBlockDevice*` 暴露给 `sys_mount` 的不是某个块设备 API,而是 `InodeOps` 的一个虚槽:

```bash
sed -n '230,236p' kernel/fs/inode.hpp        # 基类默认 nullptr
sed -n '163,164p' kernel/fs/devfs/devfs.cpp  # BlockDevOps override 返 dev_
```

基类默认 `return nullptr;`,只有 `BlockDevOps` override。grep 一下全树有多少处 override:

```bash
grep -rn "block_device.*override" kernel/ libs/ | grep -v '///'
```

> grep 末尾 `| grep -v '///'` 是过滤文档注释——不加这个过滤,`devfs.hpp:151` 那行注释(`/// InodeOps::block_device() override exposes @p dev ...`)同时含 `block_device` 和 `override` 两个词会被命中,看到 2 行容易误以为有两处 override。过滤后只剩代码行。

应见**唯一一处真 override**——`devfs.cpp:164` 的 `BlockDevOps`。其他所有 `InodeOps` 子类(`Ext2FileOps`/`TmpFileOps`/`TmpDirOps`/`ProcStatFileOps`/`DevDirOps`/...)继承默认 `nullptr`,**零代码**。这就是「加虚方法却不破坏派生类」——`sys_mount` 不需要知道 DevFS 内部结构、不 down-cast、不硬编码「如果是 DevFs 就走某条路」,全靠虚分派。

### 9. boot 真挂 + busybox mount/umount 冒烟

起真内核(test kernel 不走 boot,要起真内核才能验用户态 mount):

```bash
cmake --build build --target run
```

boot 序列应出现(章节里点过):

```
[VFS] ext2 mounted at /                # init.cpp:124
[DEVFS] mounted at /dev (N nodes)      # devfs_init.cpp:178
[PROCFS] mounted at /proc              # procfs_init.cpp:43
[TMPFS] mounted at /tmp                # tmpfs_init.cpp:48
```

进 shell 后用 busybox 做四组操作:

```bash
# (1) 运行时 sys_mount 真挂 tmpfs(081 验过,这里复用)
mount -t tmpfs none /mnt/tmp
touch /mnt/tmp/gone
ls /mnt/tmp                  # 应见 gone

# (2) umount 真回收——再 mount 同路径,文件不见了
umount /mnt/tmp
mount -t tmpfs none /mnt/tmp
ls /mnt/tmp                  # 应空——owned backend 真 delete,不是 stale 残留

# (3) 错误码冒烟——未知 fstype 返 ENODEV
mount -t nonsense none /mnt/x 2>&1; echo "exit=$?"   # 应非 0
# 注:/proc/mounts 没有,busybox mount(无参)列不出当前挂载,这是已知缺口

# (4) 块设备挂载(若有第二块盘)
# mount -t ext2 /dev/sda /mnt/disk   # 看你的 boot 注册了哪些块设备
# ls /mnt/disk                        # 应见 ext2 根目录内容
```

第 (2) 组是 owned 真回收的关键验证——`umount` 后再 `mount` 同路径,文件不见了。

> 第 (3) 组验证 errno 分工,但 busybox 的错误信息可能不直接显示 errno 名(显示「mount: ...」之类的文本)。要看精确 errno,写个小 userland 程序调 `syscall(__NR_mount, ...)` 然后 `perror` 看——或者直接信 `test_mount.cpp:87-93` 的断言(host 上跑不到 factory,kernel 测是 errno 的最硬证据)。
>
> **退路:没真块设备时造 ramdisk。** 如果你的 boot 没注册任何块设备(比如 slim boot),`test_mount_ext2_from_block_device` 的 guard 会触发 return。要 host 可复现地验 ext2 factory,可以用 `RAMBlockDevice`(`kernel/drivers/ram_block_device.hpp:36`,内存里的 `IBlockDevice` 实现)造一个 ramdisk,往里写个最小 ext2 镜像(超级块 + 块组描述符 + 一个根 inode),`BlockRegistry::register_device("sda", &ram_dev)` 注册进去,再 `devfs::init` 投影——这是 host 上验证 ext2 factory 的退路。但这需要自己搭 ext2 镜像,工作量不小,核心证据靠 kernel 测试那例(`test_mount.cpp:168-180`)+ `make run` 冒烟就够。

两腿汇总:

```bash
cmake --build build --target run-kernel-test-all 2>&1 | grep -E "Tests: [0-9]+ passed" | tail -2
```

应见单核 + `-smp 2` 两腿各 `N passed, 0 failed`。**N 须在 Book 工作树真跑后填**——基线 `CINUX_TEST_BASELINE` 默认 875(`scripts/check_test_count.sh:14`),mount 这 8 例都进 `big_kernel_test`。源仓库 dev note 记的数是源仓库的,Book 侧须独立验证,不能照抄。

## 进阶题(不给答案,留给真做过的读者)

这两题故意不给标准答案——题里点出的缺口,章节「这章没做的」已经交代过原因。lab 这儿只给场景和观察方法,答案你自己去跑出来、对照章节理解为什么会这样。

**题一:flags accepted but ignored 的边界**。写个小 userland 程序调 `mount` syscall,分别传 `flags = 0`、`flags = MS_RDONLY`(0x1)、`flags = MS_RDONLY | MS_NOSUID`(0x1 | 0x2)。挂 tmpfs 到三个不同路径,然后在每个路径下 `touch` 一个文件、`chmod +x` 看能不能执行、`echo` 看能不能写。观察三组行为有没有差别。对照章节「`MS_*` flags accepted but ignored」那一条,理解「ABI 接受」和「语义生效」的差距——为什么一个能 mount 的内核,第一步是「接受这个调用不让它崩」,第二步才是「真的按 flag 行事」。

**题二:忙挂载不返 EBUSY**。写个小 userland:`mount -t tmpfs none /mnt/x` → `touch /mnt/x/holdopen` → `open("/mnt/x/holdopen", O_RDONLY)` **不 close** → `umount("/mnt/x")`。观察返不返 `EBUSY`、发生了什么(返 0?崩溃?open 的 fd 悬垂?)。对照章节「忙挂载 umount 不返 EBUSY 直接回收」那一条,理解 Cinux 挂载表没有引用计数门的代价——Linux 是「有 fd 持有就 EBUSY」,Cinux 简化掉了。你的观察结论写不写出来都行,关键是亲手踩到这个简化。

## 验收清单

- [ ] `ctest -R vfs_mount` 见 `test_vfs_mount` 19 个 TEST 全 PASS(host 单测,覆盖挂载表纯逻辑,不覆盖 factory 四类分支)。
- [ ] `run-kernel-test` 见 mount/umount2 section(`test_mount.cpp:185`)8 例全 PASS(`test_mount.cpp:184-195`)。
- [ ] `test_mount_ext2_from_block_device`(`test_mount.cpp:168-180`)不是 skip——grep boot 日志确认 `BlockRegistry` 注册了 `sda`(`main_test.cpp:230-234` 那条 `ext2 mounted at /` 的 smoke 行),guard 不触发,8 个断言全跑。
- [ ] `grep strcmp\(fstype, kernel/syscall/sys_mount.cpp` 见 5 行命中,工厂主体 4 行(`:52` tmpfs、`:72` proc/devfs、`:73` proc 三目、`:90` ext2/ext4)+ wrapper 1 行(`:149` ext2/ext4 source-read guard);对应四类分支 + 兜底。
- [ ] `sys_mount.cpp:90-121` 块设备挂载链三层:`vfs_lookup(NoFollow)` → `block_device()` 虚方法 → `new Ext2(dev)`;`inode_unref` 平账在 `:102-104`。
- [ ] errno 分工:`test_mount.cpp:89` ext2 无 source→`EINVAL`;`:91` nonsense→`ENODEV`;`:96` umount 不存在→`ENOENT`;`sys_mount.cpp:106` 非块设备→`ENXIO`。
- [ ] `vfs_mount.hpp:38-48` MountPoint 结构 + `owned{false}` 默认;`vfs_mount.cpp:93-95` `if(owned) delete fs` 分叉;`test_mount.cpp:104-125` owned 后端真回收。
- [ ] `inode.hpp:230-236` `block_device()` 默认 nullptr;`devfs.cpp:163-164` BlockDevOps override 返 dev_;`grep block_device.*override kernel/ libs/ | grep -v '///'` 过滤文档注释后只一处真 override 命中。
- [ ] `block_registry.hpp:23-24` MAX_DEVICES=16/NAME_MAX=32;`block_registry.cpp:46` 重名拒绝注释;`devfs_init.cpp:168-172` 桥接循环。
- [ ] `make run` 见 `[VFS] ext2 mounted at /` + `[DEVFS]` + `[PROCFS]` + `[TMPFS]` 四行;busybox mount/umount 冒烟过(运行时挂 tmpfs + umount + 再挂文件不见);`run-kernel-test-all` 两腿 passed ≥ 基线 875。

## 别做这些

- **别**把 host `test_vfs_mount` 当 factory 证据——它只测挂载表纯逻辑(init/add/remove/resolve/最长前缀),**不覆盖 factory 四类分支**(那依赖内核态类,host 没有)。factory 逻辑证据靠 kernel `test_mount.cpp` 八例。
- **别**以为 `test_mount_ext2_from_block_device` 是 skip——它开头的 guard(`test_mount.cpp:169-171`)在 `run-kernel-test` boot 路径下永不触发(`main_test.cpp:230-234` 已注册 sda),8 个断言全跑真路径。注释里的「skip」是给 slim boot 留的退路。
- **别**断言「`mount -o ro` 真的只读」——`MS_RDONLY` 等 flags **accepted but ignored**(`sys_mount.cpp:46` 注释),挂出来都是可读写可执行。一个能 mount 的内核,第一步是接受调用不让它崩,第二步才是按 flag 行事——Cinux 现在在第一步。
- **别**以为 `umount2` 支持 `MNT_FORCE` 强制卸载——`flags` 标 `[[maybe_unused]]`(`sys_umount2.cpp:22`),运行期根本没有非零 flags 路径,只走 `vfs_mount_remove(target)` 一条路。注释(`:23`)明标 not yet modelled。
- **别**以为忙挂载 `umount` 返 `EBUSY`——挂载表没有引用计数门,直接回收。Linux 是「有 fd 持有就 EBUSY」,Cinux 简化掉了。
- **别**指望 `/proc/mounts` 列出当前挂载——ProcFS 动态节点扩展没做,busybox `mount`(无参)列不出。但挂载本身生效(`vfs_resolve` 能命中),用 `ls /mnt/...` 间接验。
- **别**以为 `cat /dev/sda` 能看裸字节——`BlockDevOps` 不 override read/write(`devfs.cpp:141-144` 注释),块设备靠 mount fs 消费。对比 Linux 块设备节点支持裸盘读写,Cinux 简化掉了。
- **别**混淆 `ENXIO` 和 `ENODEV`——`ENXIO`(6)是「source 解析得通但不是块设备」(`block_device()` 返 nullptr),`ENODEV`(19)是「未知 fstype」或「proc/devfs 单例未 init」。语义不同,写断言别用错。
- **别**误标 boot 挂载 `owned=true`——会让 `sys_umount2("/tmp")` 在 `vfs_mount_remove` 里 `delete` 静态对象,双重释放/崩溃。boot/static 接线永远 `owned=false`(默认 2-arg),只有 `sys_mount` 这种「对象是调用方 new 出来的」路径才传 `true`。
- **别**照抄源仓库 dev note 的测试数——那是源仓库的,Book 侧须用 `scripts/check_test_count.sh` 真跑后独立验证(基线 875,真跑后填实际数)。
- **别**把 `sys_mount -t ext4` 当成另一个类——Cinux 没有 `Ext4`,ext4 走 ext2 同一分支(`sys_mount.cpp:90` 的 `||`),`new` 的就是 `Ext2`。extent-mapped inode 靠 `i_flags & EXT4_EXTENTS_FL`(`ext2_extent.hpp:26-28`)路由。
