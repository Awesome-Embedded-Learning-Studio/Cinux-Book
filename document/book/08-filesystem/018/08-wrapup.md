---
title: 08 · 收尾:验证、没做的、小结
---

# 收尾:验证、没做的、小结

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
- `[TMPFS] mounted at /tmp`(`tmpfs_init.cpp:48`)——tmpfs 挂上(017 章验过)

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
- **`owned` bool 统一三类生命周期**(承 017)——堆 `owned=true` / 单例 `owned=false` / 未知 `ENODEV`,在 `MountPoint.owned` 一个 bool 上收敛,默认 false 兼容所有旧 2-arg 调用。`vfs_mount_remove` 一个 `if (owned) delete fs` 就是全部分叉。
- **四个 errno 精确分工**——`EINVAL`(参数缺)/ `ENOENT`(source 路径不存在)/ `ENXIO`(source 解析得通但不是块设备)/ `ENODEV`(未知 fstype 或单例未 init)。写测试断言别用错。
- **诚实边界**——`MS_*`/`MNT_*` flags accepted but ignored、无 `/proc/mounts`、忙挂载不返 `EBUSY`、`BlockDevOps` 不支持裸读裸写、mount options 字符串未解析、无 bind mount / namespace / propagation。这些是工程折中,不是漏——一个能 mount 的内核,第一步是「接受调用不让它崩」,真正的 flag 建模留到后续。

至此 VFS 的「挂载侧」全貌齐了:017 讲了 tmpfs 那一行怎么填进表,本章讲了表的填充侧全四类 + 块设备链;015 讲的是表的消费侧(`vfs_resolve` 跨挂载点 + flock + dentry cache)。三章合起来,VFS 的 finale 才算完整——`sys_mount` 接进来,`vfs_resolve` 走出去,中间那张挂载表就是它俩的握手协议。
