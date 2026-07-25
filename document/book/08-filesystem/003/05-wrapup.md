---
title: 05 · 收尾:下一站与参考
---

# 收尾:下一站与参考

到这里,内核内部已经有了一套完整的 VFS 管线:挂载表把路径交给后端、后端 lookup 出 inode、inode 通过操作表能读写。但这一切都**只存在于内核里**——用户态的程序(shell)碰不到它,没有系统调用能 `open` 一个文件。VFS 现在像一间装修好却没开门的房间。

下一站(004),我们给这间房开门:把 VFS 接到系统调用上——`sys_open`(路径解析 + 分配 fd)、`sys_read`/`sys_write`(通过 fd 找 File、调 inode 操作)、`sys_close`、`sys_getdents`(列目录)。再配上用户态 libc 的封装和 shell 的 `cat`/`ls` 命令,用户就能在 shell 里敲 `cat hello.txt`、`ls`,真正用上这一章搭的 VFS。不过那是下一篇的事,我们先把「内核有了 VFS」这个里程碑坐实。

---

## 参考

- Linux VFS 设计(`Documentation/filesystems/vfs.txt`、内核源码 `fs/*.c`):inode / 超级块 / file_operations / dentry 这套抽象是「inode + 操作表 + 挂载」思路的成熟形态,本章是它的最小化对照——用来理解设计取舍,**不**代表 Cinux 实现了 Linux 那套(dentry cache、inode cache、page cache 本章都没有)。
- 002 章 · [内核第一次认识「文件」:嵌入式 initrd ramdisk](../002/):本章 ramdisk 在 002 的「解析 ustar」基础上,加了 FileSystem 接口、条目表和 inode,两章是文件系统卷的前两阶。
- 本 tag 源码:[inode.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/inode.hpp)、[vfs_filesystem.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/vfs_filesystem.hpp)、[vfs_mount.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/vfs_mount.hpp) / [vfs_mount.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/vfs_mount.cpp)、[file.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/file.hpp) / [file.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/file.cpp)、[ramdisk.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ramdisk.hpp) / [ramdisk.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ramdisk.cpp)(本章 ramdisk 实现 FileSystem)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp)(Step 27 挂载);测试 [test_vfs_mount.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_vfs_mount.cpp)、[test_fd_table.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_fd_table.cpp)、[test_ramdisk.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_ramdisk.cpp)(本章扩了 lookup/read/readdir)。
