---
title: 04 · 调试现场与验证
---

# 调试现场与验证

003 没有 notes 文件,但 VFS 这套类型擦除 + 前缀匹配有几个经典坑,值得当调试现场。

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

「真归档、真挂载、真 lookup/read」在 QEMU 里验。机内测 [test_ramdisk.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_ramdisk.cpp) 在 002 基础上扩了一大块(这一章加了三百多行):验 `mount` 建出条目表、`lookup` 按名找到 inode、通过 `inode->ops->read` 读出文件内容、`readdir` 列出条目。跑它:

```bash
cmake --build build --target run-kernel-test
```

或直接跑完整内核,验收点是 Step 27 打出的 `[VFS] Ramdisk mounted at /`,以及 ramdisk 列出的文件清单——看到这两样,说明「挂载表 + ramdisk 实现 FileSystem + inode 可读」整条内核管线通了。这一章的难点和前两章一样:VFS 正确性靠现象间接验证(能 lookup 到、能读出内容),所以 host 数据结构测(焊死 resolve/FDTable)+ 机内测(真跑 ramdisk 全链路)缺一不可。

## 下一站
