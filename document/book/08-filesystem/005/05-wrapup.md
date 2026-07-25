---
title: 05 · 收尾:下一站与参考
---

# 收尾:下一站与参考

到这里,内核挂载的是一块**磁盘上、按 ext2 标准布局、能读**的真文件系统了——002/003 的 ramdisk 被它取代,004 的 `cat`/`ls` 不用改一行就能读 ext2 盘。但这个 ext2 是**只读**的:`ext2_file_read` 能读,却没有 `create`/`write`——你不能在 shell 里 `touch` 一个文件或写点什么。

下一站(006),给 ext2 加上**写**:分配新 inode、分配数据块、写目录项、更新位图和统计。有了写,ext2 才算个「能用的」文件系统,而不是只读快照。再往后(007 及之后)还有 `cwd`/`stat`、同步安全、init 线程模型等一串打磨。不过那是后面的事,我们先把「能从磁盘读 ext2 文件」这个里程碑坐实。

---

## 参考

- ext2 规范 — *The Second Extended Filesystem*([ext2_types.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ext2_types.hpp) 头注释已引):超块字段与 magic `0xEF53`、块组与块组描述符、inode 的 `i_block[15]`(12 直接 + 单/双/三间接)、inode 号 1-based、根目录 inode=2、变长目录项(`rec_len`/`name_len`/`file_type`)、稀疏文件。权威格式依据。社区速查可参考 [OSDev — ext2](https://wiki.osdev.org/Ext2)。
- 003 章 · [给文件一个统一接口:VFS 内核层](../003/):ext2 实现的就是 003 定义的 `FileSystem`(mount/lookup)和 InodeOps,挂进同一套 VFS——本章是 003 抽象的第一个「真」后端。
- 001 章 · [让内核自己找到磁盘:PCI 枚举与 AHCI 驱动](../001/):ext2 的所有块 I/O 都走 `ahci.read(port, lba, sectors, buf_phys)`。
- 本 tag 源码:[ext2_types.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ext2_types.hpp)、[ext2.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ext2.hpp)(本 tag 的 `Ext2` 实现集中在单个 `ext2.cpp`,后续 tag 才拆成 `ext2_block/inode/directory/init/common` 等多文件)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp)(`static Ext2 ext2(ahci, 1)` + 挂 `/`)、[create_ext2_disk.sh](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/scripts/create_ext2_disk.sh);测试 [test_ext2.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_ext2.cpp)(host 算术镜像)、[test_ext2.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_ext2.cpp)(QEMU 真盘)。
