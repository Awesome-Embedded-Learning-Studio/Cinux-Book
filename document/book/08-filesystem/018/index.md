---
title: 018 · mount/umount:把后端接到路径上的接线口
---

# 018 · mount/umount:把后端接到路径上的接线口

> 拆开运行时 mount/umount 的内部:mount factory 四类 fstype 分发、块设备挂载链、BlockRegistry + DevFS block node、InodeOps::block_device、挂载表 owned 生命周期、umount2 摘槽 vs delete 分叉。

## 本章路线

- [01 · 导引:运行时挂载的黑盒](01-intro.md)
- [02 · mount factory:四类 fstype 怎么分发](02-factory.md)
- [03 · 块设备挂载链:从 /dev/sda 到 Ext2 实例](03-block-mount.md)
- [04 · BlockRegistry + DevFS block node](04-blockregistry.md)
- [05 · InodeOps::block_device:一个设备身份槽](05-device-slot.md)
- [06 · 挂载表 MountPoint + owned 生命周期](06-mountpoint.md)
- [07 · umount2:摘槽 vs delete 的分叉](07-umount.md)
- [08 · 收尾:验证、没做的、小结](08-wrapup.md)
