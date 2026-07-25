---
title: 02 · 设计图:写的统一姿势与分配器
---

# 设计图:写的统一姿势与分配器

## 设计图

一条写链路,从 shell 命令一路钻到磁盘。以 `echo hi > /hello.txt` 为例:

```text
shell: echo hi > /hello.txt
  │  解析出 '>' 重定向,目标路径 /hello.txt
  ▼
user libc: sys_creat("/hello.txt")      [系统调用号 85]
  ▼
syscall_entry (syscall.S)               ← 栈必须 16 字节对齐!（见调试现场）
  ▼
syscall_dispatch → sys_creat
  │  ① vfs_resolve("/hello.txt") → fs = ext2, rel_path = "hello.txt"
  │  ② split_pathname → parent = "" (根), leaf = "hello.txt"
  │  ③ fs->lookup("") → 根目录 inode (ino=2)
  │  ④ parent->ops->create(...) = Ext2DirOps::create
  ▼
Ext2::create(parent_ino=2, "hello.txt")
  │  ① lookup_in_dir 查重
  │  ② alloc_inode()        ← 扫 inode 位图,分一个新 inode 号
  │  ③ 初始化 Ext2Inode (REG|0644, links=1)
  │  ④ write_disk_inode()   ← 读-改-写:把新 inode 写回 inode 表所在块
  │  ⑤ add_dir_entry()      ← 在根目录数据块里插入 "hello.txt" 目录项
  │  ⑥ write_disk_inode(根) ← 根目录 size/block 可能变了,写回
  ▼
（随后 sys_open + sys_write 把 "hi" 写进文件的数据块,同样 read-modify-write）
```

贯穿全章的,是「读—改—写」这个写回姿势。块是 1024 字节一个的整体,但 inode 只有 128 字节、BGDT 表项 32 字节、位图里一个位——它们都比块小。AHCI 只会按块(其实是按扇区)整块整块地搬,你没法只写「一个 inode」。于是 ext2 的每一次元数据修改都是这三步:

```text
          ┌─────────────────────────────────────────┐
读 read_block(N) ──▶ │  那块唯一的 DMA 缓冲 (一页)              │
          └─────────────────────────────────────────┘
                              │ 在缓冲里改掉要改的几个字节
                              ▼
          ┌─────────────────────────────────────────┐
写 write_block(N) ◀── │  改完的整块,经 ahci.write 落盘           │
          └─────────────────────────────────────────┘
```

这是 005 那块「单 DMA 缓冲」(一次只装一块、用完即覆盖)的直接后果,也是本章所有写代码的底色。

## 代码路线
