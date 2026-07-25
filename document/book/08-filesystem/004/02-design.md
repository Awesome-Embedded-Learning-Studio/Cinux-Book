---
title: 02 · 设计图:系统调用怎么落到 VFS
---

# 设计图:系统调用怎么落到 VFS

## 设计图

系统调用把用户态的「文件操作」翻译成 VFS 的「inode 操作」。看 open + read 这条主链:

```text
   用户态: sys_open("/hello.txt", O_RDONLY)
        │  (libc 包装:一句 syscall 指令,参数进 rdi/rsi/rdx)
        ▼  syscall 陷入内核 ── 派发表 ──▶ sys_open(path_virt, flags, ...)
   sys_open:
        ① 规范地址检查(path_virt 合法、非空、非内核态地址)
        ② fs = vfs_resolve(path, &rel_path)      ← 003 的挂载表
        ③ inode = fs->lookup(rel_path)            ← 后端按名找
        ④ fd = g_global_fd_table().alloc(inode, flags)   ← 分配描述符
        return fd  (≥3)  或 -1

   用户态: sys_read(fd, buf, 256)
        ▼  sys_read(fd, buf_virt, count):
        ① 规范地址检查(buf_virt)
        ② if fd==0: 走老路读键盘(stdin)        ← 保留的特殊路径
           else:
              file = FDTable.get(fd)
              n = file->inode->ops->read(inode, file->offset, buf, count)  ← 调到后端
              file->offset += n                              ← ★ VFS 层推进偏移
        return n
```

关键在两层分工:**系统调用层**负责「fd 状态 + 偏移推进 + 地址安全」这些和具体文件系统无关的事;**inode 操作层**(003 的 InodeOps)负责「从哪读、怎么读」这些后端特定的事。sys_read 调完 `ops->read` 后自己把 `file->offset` 加上读到的字节数——偏移是 VFS 层管的,后端的 read 只管「从 offset 处给我 count 字节」,不管「下次从哪接着读」。

`getdents`(列目录)复用了一个巧妙的设计:

```text
   sys_getdents(fd, buf, count):
        file = FDTable.get(fd)
        n = file->inode->ops->readdir(inode, file->offset, buf, count)   ← offset 当条目下标用
        if n==1: file->offset++ ; return 名字长度        ← 下标 +1,下次读下一条
        return n   (0 = 目录读完)
```

`file->offset` 在这里**兼当目录条目的下标**——read 用它当字节偏移,getdents 用它当条目序号。同一个字段两种语义,因为对目录而言「偏移」就是「第几个条目」。这样 shell 的 `ls` 只要循环 getdents 到返回 0,就能把目录列完。

## 代码路线
