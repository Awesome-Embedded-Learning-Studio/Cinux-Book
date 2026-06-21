---
title: 参考 · 文件系统:VFS、ramdisk、ext2 与文件描述符
---

# 参考 · 文件系统:VFS、ramdisk、ext2 与文件描述符

> 查阅层。这一页是 Cinux 文件系统子系统的速查表,不按 tag 组织,给后续章节(ramdisk 026、VFS 027、ext2 028 系列、cwd/stat 028c、init 线程 028e、管道 031b、shell 024……)查 VFS 抽象、Inode/Ops、FDTable、ext2 布局、syscall 号用。实现以最终 tag `035_multi_terminal` 源码为准。
>
> 范围:VFS 三层抽象(FileSystem → Inode+InodeOps → File+FDTable)、USTAR ramdisk、ext2 只读 + 后期可写、POSIX 风格 fd 与 syscall。**不含 journaling、不含符号链接解析的完整实现、不含权限强制(早期 tag 多为 stub)**。

## 子系统地图

```text
   用户态 shell / 进程
        │  syscall(syscall_nums.hpp): SYS_read=0 SYS_write=1 SYS_open=2
        │   SYS_close=3 SYS_stat=4 SYS_pipe=22 SYS_chdir=12 SYS_getcwd=79 ...
        ▼
   ┌────────────────────────────────────────────────────────────┐
   │  sys_read / sys_write 派发:先查 FDTable → File → Inode.ops   │
   │   命中 ops 就走 VFS;否则回退 fd0=键盘 / fd1=串口(031b 的翻转) │
   ▼                                                            │
   FDTable(256 槽,fd 0/1/2 = stdin/out/err 外部装)              │
        │ get(fd) → File(inode, offset, flags)                   │
        ▼                                                        │
   Inode(ino, size, type, mode, nlink, times, ops*)              │
        │ ops->read/write/readdir/create/mkdir/unlink/stat       │
        ▼                                                        │
   FileSystem 抽象(mount/lookup)                                 │
        ├── Ramdisk(USTAR tar,512B 块,magic "ustar")            │
        └── Ext2(superblock 0xEF53 @1024,inode 128B,15 block ptr)│
                                                                   ▼
   管道(031b):包成 Inode,挂 PipeReadOps / PipeWriteOps,统一进 VFS
```

设计核心:**一切皆 Inode**——普通文件、目录、设备、管道都先变成带 `InodeOps` 虚表的 `Inode`,`sys_read`/`sys_write` 只对着 `Inode.ops` 调用,不关心后端是什么。这就是管道(031b)能「包成 Inode」复用整套 VFS 的原因。

## VFS 三层抽象

| 层 | 类 / 结构 | 职责 |
|---|---|---|
| 后端 | `FileSystem`(抽象,`vfs_filesystem.hpp`) | `mount()`、`lookup(path) → Inode*`;ramdisk/ext2 各自实现 |
| 节点 | `Inode` + `InodeOps`(`inode.hpp`) | 一个文件/目录的元数据 + 操作虚表 |
| 句柄 | `File` + `FDTable`(`file.hpp`) | 打开实例(inode + offset + flags)+ 进程的 fd 表 |

## Inode 与 InodeOps

`Inode`(`inode.hpp`)字段:`ino`、`size`、`type`(`InodeType`:Regular/Directory/…)、`ops*`(虚表,可空)、`mode`、`uid`、`gid`、`nlink`、`atime/ctime/mtime`、`blocks`。

`InodeOps` 是抽象基类(虚表),后端按需 override:

| 方法 | 默认 | 说明 |
|---|---|---|
| `read(inode, offset, buf, count) → 字节数` | 不支持返负 | 读 |
| `write(inode, offset, buf, count) → 字节数` | 不支持返负 | 写(早期 tag ramdisk 只读) |
| `readdir(inode, index, name, namemax) → 长度` | — | 按索引枚举目录项 |
| `create(dir, name, namelen) → Inode*` | nullptr | 建文件 |
| `mkdir(dir, name, namelen) → Inode*` | nullptr | 建目录 |
| `unlink(dir, name, namelen)` | — | 删除 |
| `stat(inode, struct stat*)` | — | 填状态 |

每个具体 FS(ramdisk/ext2/pipe)提供自己的 `InodeOps` 子类,`Inode::ops` 指向它。`ops == nullptr` 表示该 inode 无操作(纯数据)。

## File 与 FDTable

`File`(`file.hpp`):`inode*`、`offset`、`flags`(`OpenFlags`),`offset` 有 `Spinlock` 保护(多 fd 共享同一 inode、各自 offset 独立)。

`FDTable`:

| 项 | 值 / 说明 |
|---|---|
| `FD_TABLE_SIZE` | **256**(固定数组) |
| `FD_NONE` | `-1`(未分配哨兵) |
| fd 0 / 1 / 2 | stdin / stdout / stderr,**外部**装(init.cpp / sys_pipe 用 `set`) |
| `alloc(inode, flags) → fd` | 找第一个**空闲**槽,建 File 返回 fd(**跳过 0/1/2**,因它们是预留) |
| `set(fd, file)` | **强制**装到指定 fd(覆盖),sys_pipe / dup2 / 绑 fd0/fd1 用 |
| `get(fd) → File*` | 取(nullptr = 非法/未用) |
| `close(fd) → 0/-1` | 释放 File |

FDTable 嵌在 `Task` 里,每任务一张;`Task::fd_table == nullptr` 表示共享全局内核表。生命周期:FDTable 拥有 File 对象,`close` 释放。

> **关键设计(031b 的坑):** `sys_read`/`sys_write` 的派发顺序是「先查 FDTable 走 Inode.ops、再回退 fd0 键盘 / fd1 串口」。如果把顺序写反(先 fd0/fd1),管道会被短路——数据打到串口而不是管道。

## ramdisk 后端(USTAR)

ramdisk 把一块内嵌的 **USTAR tar** 镜像当文件系统(`ramdisk_config.hpp`):

| 项 | 值 |
|---|---|
| 块大小 | `USTAR_BLOCK_SIZE = 512`(`UstarHeader` 恰 512 字节) |
| magic | `"ustar"` |
| type flag | `'0'` Regular、`'5'` Directory、`'1'` Hardlink、`'2'` Symlink、`'6'` FIFO、`'3'`/`'4'` 字符/块设备、`'7'` Contiguous |

ramdisk 在 026 引入,是 ext2 上线前让 shell 能读文件的简单后端;**只读**(无 write ops,或 write 返回不支持)。镜像在编译期嵌进内核镜像。

## ext2 后端

`kernel/fs/ext2*`(superblock / inode / directory / block / init):

| 项 | 值 |
|---|---|
| `EXT2_SUPER_MAGIC` | `0xEF53` |
| superblock 位置 | 偏移 **1024**(块 0 保留),大小 1024 字节 |
| inode 默认大小 | 128 字节(`EXT2_INODE_SIZE_DEFAULT`) |
| 块指针 | `i_block[15]`:12 直接 + 1 间接(`EXT2_INDIRECT_BLOCK=12`)+ 1 二级间接(13)+ 1 三级间接 = 共 `EXT2_TOTAL_BLOCK_PTRS=15` |
| 文件名 | 最长 `EXT2_NAME_MAX = 255` |
| 目录项 | 头 8 字节(`EXT2_DIR_ENTRY_HDR_SIZE`),name 从偏移 8 起 |
| inode 缓存 | `EXT2_INODE_CACHE_SIZE = 64` |
| 扇区 | `EXT2_SECTOR_SIZE = 512`(经 AHCI 读写) |
| mode 掩码 | `EXT2_S_IFMT=0xF000`、`EXT2_S_IFREG=0x8000`、`EXT2_S_IFDIR=0x4000` |

ext2 经 028(只读挂载解析)→ 028b(写)→ 028c(cwd/stat)→ 028d(sync 安全)→ 028e(init 线程)逐步补齐。读写裸扇区走 [storage-ahci-pci.md](storage-ahci-pci.md) 那套 AHCI DMA。

## 系统调用(syscall_nums.hpp)

| 号 | 名 | 说明 |
|---|---|---|
| 0 | `SYS_read` | 读 fd(派发见上) |
| 1 | `SYS_write` | 写 fd |
| 2 | `SYS_open` | 打开路径 → fd |
| 3 | `SYS_close` | 关 fd |
| 4 | `SYS_stat` | 取 inode 状态 |
| 12 | `SYS_chdir` | 改当前目录 |
| 22 | `SYS_pipe` | 建管道,返两个 fd(031b) |
| 39 | `SYS_getpid` | 取 pid |
| 57 | `SYS_fork` | 复制进程(034/035) |
| 59 | `SYS_execve` | 替换镜像 |
| 60 | `SYS_exit` | 退出 |
| 61 | `SYS_waitpid` | 收尸 |
| 79 | `SYS_getcwd` | 取当前目录 |

`struct stat`(`stat.hpp`):`st_dev/st_ino/st_mode/st_nlink/st_uid/st_gid/st_rdev/st_size/st_blksize/st_blocks/st_atime/st_mtime/st_ctime`。

## 约束与边界(本子系统的真实限制)

- **fd 表固定 256。** `alloc` 跳过 0/1/2(预留),所以第一个普通 fd 是 3;装固定 fd 用 `set`。
- **ramdisk 只读。** 写文件要 ext2(028b+);早期 shell 对 ramdisk 只能读。
- **管道 = 带 PipeReadOps/PipeWriteOps 的 Inode**(031b)。它复用 VFS,但派发顺序错了会被 fd0/fd1 回退短路。
- **权限(uid/gid/mode)多为元数据,早期不强制。** mode 字段存着,但 open/execute 不一定检查(核对具体 tag)。
- **符号链接/硬链接解析程度有限。** USTAR/ext2 都定义了类型,但完整路径解析(尤其 symlink 跟随)未必全实现。
- **无 journaling、无 fsck。** 028d 的 sync 安全是「写后刷盘、防掉电损坏」的朴素版本,不是日志。

## 验证入口

- host 单测:`ctest --test-dir build -R "vfs|inode|ext2|ramdisk|fd|file|path" --output-on-failure`。
- QEMU 机内测:`cmake --build build --target run-big-kernel-test`(`kernel/test/test_shell.cpp` 端到端跑 sys_read/write/open、管道、shell 命令)。
- 可视化:`cmake --build build --target run`,shell 里 `ls`/`cat`/`cd` 验证 VFS + ext2。

## 源码索引

- VFS 抽象:[vfs_filesystem.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/vfs_filesystem.hpp) / [vfs_mount.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/vfs_mount.hpp) / [vfs_mount.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/vfs_mount.cpp)。
- Inode:[inode.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/inode.hpp) / [inode.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/inode.cpp)。
- File/FD:[file.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/file.hpp) / [file.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/file.cpp)。
- ramdisk:[ramdisk.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ramdisk.hpp) / [ramdisk.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ramdisk.cpp) / [ramdisk_config.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ramdisk_config.hpp)。
- ext2:[ext2.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ext2.hpp) / [ext2_types.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ext2_types.hpp) / [ext2_init.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ext2_init.cpp) / [ext2_inode.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ext2_inode.cpp) / [ext2_directory.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ext2_directory.cpp) / [ext2_block.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ext2_block.cpp)。
- 路径/stat:[path.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/path.hpp) / [stat.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/stat.hpp)。
- syscall:[syscall_nums.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/syscall_nums.hpp) + `kernel/syscall/` 派发。

## 权威依据

- ext2 规范(superblock 0xEF53、inode 128B、i_block[15]=12 直接+间接+二级+三级、目录项线性布局):源设计文档;OSDev — [Ext2](https://wiki.osdev.org/Ext2)。
- POSIX / Linux man-pages(`open(2)`/`read(2)`/`stat(2)`/`pipe(2)`/`chdir(2)`,fd 0/1/2 约定):syscall 语义与 fd 分配规则。<https://man7.org/linux/man-pages/>
- USTAR tar 格式(PAX/ustar header、type flag、magic "ustar"):POSIX.1-1988 (ustar) / [Wikipedia tar](https://en.wikipedia.org/wiki/Tar_(computing))。
- 「一切皆 Inode」设计对比:Unix V6/V7 inodes、xv6 `struct inode` + `itable`(<https://github.com/mit-pdos/xv6-riscv>)。
