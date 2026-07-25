---
title: 01 · 导引:运行时挂载的黑盒
---

# 导引:运行时挂载的黑盒

> 做完之后,`sys_mount -t ext2 /dev/sda /mnt` 真能挂上一块盘,`umount2 /mnt` 能干净回收;`MS_RDONLY` 之类的 flags 收下但忽略、忙挂载不返 `EBUSY`、没有 `/proc/mounts` 可查表——这些边界先说在前头,以源码为准,不假装做了。

## 这章咱们要点亮什么

1. **mount factory 是 fstype 驱动的四类分发**——`do_mount_kernel` 按 `fstype` 字符串走路由:tmpfs / proc·devfs / ext2·ext4 / 未知返 `ENODEV`。跟 017 tmpfs 章是**同一张表的不同行**:017 只把 tmpfs 那一行展开了,这里看全表。
2. **三类 owned 策略在一个 bool 上收敛**(承 017)——堆 `owned=true` / 单例 `owned=false` / 未知 `ENODEV`,全在 `MountPoint.owned` 这一个字段上。
3. **块设备挂载链是 015 deferred 的核心**——source 路径 → `vfs_lookup(NoFollow)` 拿 `Inode` → `InodeOps::block_device()` 虚方法抽 `IBlockDevice` → `new Ext2(dev)`,三层解析把一个字符串变成一个挂好的 FS。
4. **四个 errno 的精确分工**——`EINVAL`(参数缺)/ `ENOENT`(source 路径不存在)/ `ENXIO`(source 解析得通但不是块设备)/ `ENODEV`(未知 fstype 或单例未 init)。
5. **诚实边界——flags accepted but ignored**——`MS_*` / `MNT_*` 全收下不解析,`/proc/mounts` 没有,忙挂载不返 `EBUSY`,`BlockDevOps` 不支持裸盘读写。这些是工程折中,不是漏。
