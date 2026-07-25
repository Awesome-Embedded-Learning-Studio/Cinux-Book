---
title: 05 · 收尾:下一站与参考
---

# 收尾:下一站与参考

到这里,从用户敲 `cat hello.txt` 到 ramdisk 里读出文件内容的整条路通了:系统调用把文件操作接进了 VFS,VFS 派发给 ramdisk 后端,数据回到用户态。但你会发现这套文件系统有个硬限制:它是**只读**的(ramdisk 的 write 恒 -1),而且数据是构建期嵌进内核的、不是从磁盘上读的。它证明「文件抽象」成立,但不是个能持久化、能写的真文件系统。

下一站(005),我们做真正的文件系统——ext2:它活在 001 那块 AHCI 磁盘上,能读也能写、有块分配、有目录层级。003/004 这两篇搭的 VFS 抽象(inode、操作表、挂载、系统调用)正好是它的地基:ext2 只要实现 `FileSystem` 接口,就能挂进同一个 VFS,用户态的 `cat`/`ls` 一行不改就能用上它。不过那是下一章的事,我们先把「用户态能 open/read 文件」这个里程碑坐实。

---

### 参考

- Linux man-pages — [`open(2)`](https://man7.org/linux/man-pages/man2/open.2.html)、[`read(2)`](https://man7.org/linux/man-pages/man2/read.2.html)、[`getdents(2)`](https://man7.org/linux/man-pages/man2/getdents.2.html):系统调用语义对照。Cinux 这版是简化形态(如 getdents 一次一条、open 的 flags 只有 RDONLY/WRONLY/RDWR),POSIX 的完整语义(创建、权限、一次多条结构体)本章没实现,别拔高。
- Intel SDM Vol.1 — Canonical Address:x86-64 虚拟地址的「规范形」规则(bit 47 决定用户/内核半区,bit 48–63 必须与 bit 47 一致),这是 sys_open 等做地址合法性检查的硬件依据。本地 PDF `document/reference/intel/SDM-Vol3A-*.pdf`,可搜 "Canonical" 复核。
- 003 章 · [给文件一个统一接口:VFS 内核层](../003/):这一篇接的就是 003 搭的 VFS 管线(inode、FileSystem、挂载表、FDTable)。两篇是同一个 tag 的上下半。
- 本 tag 源码:[sys_open.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/sys_open.cpp) / [sys_read.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/sys_read.cpp) / [sys_getdents.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/sys_getdents.cpp) / [sys_close.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/sys_close.cpp)、[syscall_nums.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/syscall_nums.hpp)、[syscall.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/libc/syscall.cpp)、[cmd_cat.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/programs/shell/cmd_cat.cpp) / [cmd_ls.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/programs/shell/cmd_ls.cpp);测试 [test_vfs_syscall.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_vfs_syscall.cpp)、[test_fd_table.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_fd_table.cpp)。
