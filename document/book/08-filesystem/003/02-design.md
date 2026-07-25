---
title: 02 · 设计图:VFS 四件套
---

# 设计图:VFS 四件套

## 设计图

VFS 把「一个路径」变成「一次读」,中间经过几层。看完整链路:

```text
   用户/上层拿一个路径 "/hello.txt"
        ▼
   vfs_resolve(path, &rel_path)          ← 查挂载表,最长前缀匹配
        │   遍历 MountPoint[]:找 path 的最长前缀
        │   边界判定:匹配必须落在路径分量边界(后跟 '/' 或 '\0')
        ▼
   FileSystem* fs  +  rel_path("hello.txt")   ← 剥掉挂载前缀后的相对路径
        ▼
   fs->lookup(rel_path)                      ← 后端按名找,返回 Inode*
        ▼
   Inode { ino, size, type, ops, fs_private }
        │   ops 指向静态 InodeOps 表
        ▼
   inode->ops->read(inode, offset, buf, count)   ← 通过函数指针调到后端的读
        │   后端的 read 用 inode->fs_private 找回自己的私有数据(如 RamdiskEntry)
        ▼
   数据进 buf
```

打开文件状态(`File` / `FDTable`)是这条链路之外的另一条线,管「这次打开,读到哪了」:

```text
   sys_open(将来):resolve+lookup 得到 inode → FDTable.alloc(inode,flags) → 返回 fd
   FDTable[fd] = File{ inode, offset=0, flags }
        │   多个 fd 可指向同一 inode,各自 offset 独立
   sys_read(将来):fd → FDTable.get(fd) → file->inode->ops->read(inode, file->offset, ...)
                  → 读到 buf,file->offset += n
```

两条线在「inode」上汇合:VFS 链路负责「路径 → inode」,FDTable 链路负责「inode + 打开状态 → 多次读写」。这一章两条线都搭好(只是还没接系统调用)。

## 代码路线
