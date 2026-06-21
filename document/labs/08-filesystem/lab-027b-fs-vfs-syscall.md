---
title: Lab 027b · 让用户态能 open/read 文件:VFS 系统调用与 shell
---

# Lab 027b · 让用户态能 open/read 文件:VFS 系统调用与 shell

> 配套章节:[027b · 让用户态能 open/read 文件](../../book/08-filesystem/027b-fs-vfs-syscall.md)。这一关给你目标和约束,不贴 sys_open 的解析流程、不贴 libc 的 syscall 汇编包装、不贴 cat/ls 的循环骨架。前提是 Lab 027 的 VFS 内核层已经搭好。

## 实验目标

把 027 搭的 VFS 接到系统调用上,让用户态真能 open/read 文件。拆成三个子目标:

1. 系统调用层:实现 `sys_open`(路径→inode→fd)、`sys_read`/`sys_write`(fd→File→inode 操作)、`sys_close`、`sys_getdents`,接进 syscall 派发表。
2. 用户态封装:libc 里给每个系统调用写汇编包装(一句 `syscall` + 装参数),让用户程序像调函数一样用。
3. shell 落地:`cat`(open+read 循环+写 stdout)和 `ls`(open 目录+getdents 循环)。

做完这三条,进 shell 敲 `cat hello.txt` 能看到内容、`ls` 能列目录,从键盘到 VFS 到 ramdisk 的整条链路就通了。

## 前置条件

过 Lab 027:VFS 内核层(inode、FileSystem、挂载表、FDTable、ramdisk 后端)已就绪——这一关直接用 `vfs_resolve`、`fs->lookup`、`g_global_fd_table()`。

过 023 的 syscall 框架:`syscall` 指令、派发表、`SyscallNr` 编号都懂。这一关是往那张表里加新条目。

## 任务分解

**第一步:sys_open。** 活是「路径 → inode → fd」。先做规范地址检查(用户传的 path 指针必须在用户态低半区,想清楚怎么用 bit47 判、为什么不能信用户传的内核地址)。然后 vfs_resolve 找后端、fs->lookup 找 inode、FDTable.alloc 分配 fd。每一步失败都要返回 -1。flags 映射成访问模式。

**第二步:sys_read / sys_write。** read 按 fd 分两条路:fd==0 走老路读键盘(stdin 不是 VFS 文件,想清楚为什么——FDTable 里 fd 0 没有 File),fd>0 走「get(fd) 拿 File → inode->ops->read(inode, file->offset, buf, count) → 推进 file->offset」。那行推进 offset 是灵魂,想清楚漏了会怎样(反复读开头)。write 同理收编进 VFS。

**第三步:sys_close / sys_getdents。** close 就是 FDTable.close(fd)。getdents 列目录:关键是 `file->offset` 在这里当**目录条目下标**用(不是字节偏移)——readdir(inode, file->offset, buf, count) 读一条,读到就 offset++、返回名字长度,循环到返回 0 表示目录读完。想清楚为什么 offset 能这么复用(对目录,「偏移」就是「第几条」)。

**第四步:libc 包装 + shell。** 给每个系统调用写个内联汇编包装:syscall 号进 rax、参数进 rdi/rsi/rdx、一句 `syscall`、返回值从 rax 取。记得把 `rcx`/`r11` 加进 clobber(`syscall` 指令会破坏它们)。然后 shell 加 cat(open+read 循环到 ≤0+写 stdout+close)和 ls(open 目录+getdents 循环到 ≤0+每个名字换行+close)。两个命令都是「open 拿 fd → 循环到读完 → close」的骨架,offset 的推进全在内核,用户程序不用管「读到哪了」。

## 接口约束

你要实现出来的东西,对外长这样(职责和签名,不给实现):

- `sys_open(path_virt, flags, ...) -> int64_t`:规范地址检查 → resolve → lookup → alloc fd。
- `sys_read(fd, buf_virt, count, ...) -> int64_t`:fd==0 读键盘;fd>0 走 VFS 并推进 offset。
- `sys_write` / `sys_close(fd)` / `sys_getdents(fd, buf_virt, count, ...)`:同上,offset 当条目下标。
- libc:`sys_open/read/write/close/getdents` 各一个 `syscall` 汇编包装。
- shell:`cmd_cat`(open+read 循环+write stdout)、`cmd_ls`(open+getdents 循环)。

硬约束:

- 每个收用户指针的调用(open 的 path、read/write/getdents 的 buf)**必须**做规范地址检查。
- read 完必须推进 `file->offset`;getdents 用 offset 当条目下标、每读一条 offset++。
- fd 0 的 read 保留键盘老路,不进 VFS;VFS 只服务 fd≥3。
- 这一关**不**实现文件创建/写盘(ramdisk 只读);getdents 是「一次一条」的简化形态,不是 POSIX 那种一次多条结构体。

syscall 号怎么编、汇编约束怎么写、cat/ls 的循环怎么组织,都得你照 023 的 syscall 约定来定,这关不提供。

## 验证步骤

FDTable 在 host 上镜像测(Lab 027 已有)。系统调用接 VFS 的行为在 QEMU 里验。机内 vfs 系统调用测试(近 700 行)测 open 找到文件、read 读出内容、close 释放、getdents 列目录,以及错误路径(文件不存在、fd 非法、表满):

```bash
ctest --test-dir build -R 'fd_table' --output-on-failure      # host
cmake --build build --target run-kernel-test                  # QEMU 机内测
```

最直观的验收是跑起来进 shell:`cat hello.txt` 应输出文件内容,`ls` 应列出 initrd 里的文件名。这两条命令通,整条用户态→syscall→VFS→ramdisk 链路就通了。

## 常见故障

- **用户传内核地址,内核替它读了内核内存**:规范地址检查漏了或写反。每个收用户指针的调用开头都要那道 bit47 判定。
- **stdin(fd 0)读不到键**:sys_read 把 fd 0 也走了 VFS,但 FDTable 里 fd 0 没有 File,get(0) 返 nullptr → read -1。fd 0/1/2 得保留键盘/屏幕老路,只 fd≥3 走 VFS。
- **`cat` 死循环打印文件开头那几字节**:read 完没推进 `file->offset`,每次都从 0 读。后端 read 无状态,推进偏移是 VFS 层的活。
- **`ls` 死循环在第一个条目**:getdents 忘了 `file->offset++`,反复读同一条。对目录 offset 是下标不是字节偏移。
- **写到 ramdisk 没生效**:ramdisk 只读,write 恒 -1。调用方得检查 write 返回值,别假定成功。

## 通过标准

1. 机内 vfs 系统调用测试全绿:open/read/close/getdents 的正常路径和错误路径(文件不存在、fd 非法、表满)。
2. 进 shell:`cat hello.txt` 输出文件内容,`ls` 列出目录条目。
3. 每个收用户指针的调用都做规范地址检查;read 完推进 offset;getdents 用 offset 当条目下标。
4. fd 0 的 read 走键盘老路,fd≥3 走 VFS;libc 包装把 rcx/r11 加进 clobber。
5. 不实现写盘/文件创建(ramdisk 只读),getdents 是一次一条的简化形态。

做到这五条,从用户键盘到 VFS 到 ramdisk 的整条路就通了。但文件系统还是只读、数据还嵌在内核里——下一关(028 ext2)才做磁盘上能读能写的真文件系统。
