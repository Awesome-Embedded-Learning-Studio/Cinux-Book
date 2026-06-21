---
title: Lab 027 · 给文件一个统一接口:VFS 内核层
---

# Lab 027 · 给文件一个统一接口:VFS 内核层

> 配套章节:[027 · 给文件一个统一接口:VFS 内核层](../../book/08-filesystem/027-fs-vfs.md)。这一关给你目标和约束,不贴 `InodeOps` 函数指针表、不贴 `vfs_resolve` 的边界判定、不贴 ramdisk 实现 `FileSystem` 的代码。这一篇只做内核内部的 VFS 管线,系统调用和 shell 在 Lab 027b。

## 实验目标

在内核里搭一套「与后端无关」的文件抽象,让 ramdisk 实现它、挂上来。拆成四个子目标:

1. 定义 inode 抽象:文件对象(`Inode`)+ 一组操作(`InodeOps` 函数指针表)。
2. 定义文件系统后端接口(`FileSystem`:mount/lookup)+ 一张挂载表(路径前缀 → 后端)。
3. 实现「打开文件」状态(`File`)+ 文件描述符表(`FDTable`)。
4. 让 ramdisk 实现这套接口:mount 建条目表 + 预分配 inode,lookup 按名找,提供 read/readdir。

做完这四条,内核就能拿一个路径,解析出文件对象、读出内容——但这一关还不接系统调用(用户态碰不到)。

## 前置条件

过 Lab 026:ramdisk 能解析 ustar、列出文件。这一关让那份 ramdisk「升级」成真正的文件系统后端。

要对「函数指针表当虚表用」这种 C 风格多态有概念——这一关 inode 的操作就是这么做的(不是 C++ 虚函数)。想清楚为什么:inode 数量多,不能每个背虚表指针;操作表做成静态、同类共享。

## 任务分解

**第一步:inode 与 InodeOps。** 定义文件对象结构(编号、大小、类型、操作表指针、一个不透明私有指针)。操作表是三个函数指针(read/write/readdir)。想清楚那个不透明私有指针干嘛用的——后端的 read 怎么通过它找回自己的数据(比如「这个文件的数据在归档哪里」)。这步的关键是接受「手工虚表 + 类型擦除」这套,而不是给 inode 加虚函数。

**第二步:FileSystem 接口 + 挂载表。** 抽象基类,两个纯虚方法(mount 初始化后端、lookup 按相对路径找文件)。注意这里用 C++ 虚函数是合理的(后端就那么几个),和 inode 用函数指针表是两种选择——想清楚为什么这么分。再写一张定长的挂载表:每项存「路径前缀 + 后端指针 + 是否占用」。提供加/删/解析三个操作。解析要做**最长前缀**匹配,而且匹配必须落在**路径分量边界**——想清楚 `/fo` 为什么不能匹配 `/foo`(差一道边界判定)。

**第三步:File 与 FDTable。** 「打开的文件」结构:指向 inode、记一个读写偏移、记访问模式。关键想清楚为什么 inode 和偏移要分开存(同一个文件被 open 两次,偏移各自独立)。文件描述符表:定长数组存 File 指针,提供分配/关闭/取三个操作。分配时从 fd **3** 起算——0/1/2 留给 stdin/stdout/stderr,想清楚为什么得跳过。

**第四步:ramdisk 实现 FileSystem。** 这是把前三步的骨架长上肉。mount 不再只打印,而是**建一张条目表**:每个文件一项,拷贝名字、记大小、记数据指针(指向归档里那块数据)。关键是**每个条目顺手把它的 inode 预分配好**——设好类型、指向静态操作表、私有指针指回这个条目。再建一个根目录 inode 支持列目录。lookup:根路径返回根目录 inode,否则剥掉前导 `/` 在表里线性搜名字。提供 read(从归档数据拷贝,按大小截断)、readdir(第 0 项 `.`、第 1 项 `..`、之后是文件条目)。注意 ramdisk 是**只读**的——write 恒返回 -1,想清楚为什么(数据嵌在内核镜像里)。

## 接口约束

你要实现出来的东西,对外长这样(职责和签名,不给实现):

- `Inode { ino, size, type, ops, fs_private }` + `InodeOps { read, write, readdir }`(函数指针)。
- `class FileSystem { virtual bool mount(); virtual Inode* lookup(const char* path); }`。
- `vfs_mount_init/add/remove`、`vfs_resolve(path, &rel_path) -> FileSystem*`(最长前缀 + 边界判定)。
- `File { inode, offset, flags }` + `FDTable::alloc/close/get`(alloc 从 fd 3 起)。
- `Ramdisk : FileSystem`:mount 建表 + 预分配 inode,lookup 按名找。

硬约束:

- inode 操作用**函数指针表**(静态、同类共享),不用 C++ 虚函数;私有数据靠 `fs_private` 不透明指针串联。
- `vfs_resolve` 必须做最长前缀 + 路径分量边界判定;FDTable 从 fd 3 起算(0/1/2 预留)。
- ramdisk 只读(write 返回 -1);lookup 是扁平线性搜,把整盘当一个目录(不做多级目录遍历)。
- 这一篇**不**接系统调用、不碰用户态——那是 Lab 027b。

字段布局、操作表内容、resolve 的边界写法、条目表结构,都得你照设计来定,这关不提供。

## 验证步骤

挂载表 resolve 和 FDTable 是纯数据结构,在 host 上镜像测。resolve 验最长前缀、边界判定(`/fo` 不匹配 `/foo`)、add/remove;FDTable 验分配从 3 起、close 回收、表满:

```bash
ctest --test-dir build -R 'vfs_mount|fd_table' --output-on-failure
```

真归档、真挂载、真 lookup/read 在 QEMU 里验。机内 ramdisk 测试(本章扩了几百行)验:mount 建出条目表、lookup 按名找到 inode、通过 `inode->ops->read` 读出文件内容、readdir 列出条目:

```bash
cmake --build build --target run-kernel-test
```

或直接跑完整内核,验收点是 `[VFS] Ramdisk mounted at /`——看到这行,说明挂载表 + ramdisk 实现 FileSystem + inode 可读的整条内核管线通了。

## 常见故障

- **查 `/foo` 解析到了错误的后端 / 多挂载点路由错**:resolve 只做了 `strncmp` 前缀匹配,没做路径分量边界判定。`/fo` 就会误匹配 `/foo`。加上「匹配点后必须是 `/` 或 `\0`(或前缀以 `/` 结尾)」的判定。
- **read 返回 -1 或读到垃圾**:mount 建条目时忘了设 `inode->fs_private`(或设错),read 里的 `static_cast` 取回的是错的数据。ops 和 fs_private 必须成对设对。
- **同一个文件 open 两次,偏移互相串扰**:File 没把 inode 和 offset 分开存,或两个 fd 共享了一个 File。每个 fd 各有自己的 File、各自的 offset。
- **新打开的文件抢了 stdin/stdout 的位置**:FDTable 从 fd 0 起分配了。必须从 3 起,0/1/2 留给标准流。
- **挂载后没多久 inode 全成野指针**:Ramdisk 对象不是 `static` 的,函数返回就析构,挂载表和 inode 里那些指针全悬空。对象被外部以裸指针引用时,生命周期必须长于所有引用——这里靠 static 保活。

## 通过标准

1. host 单测全绿:resolve 的最长前缀/边界/add/remove;FDTable 的分配(从 3 起)/close/表满。
2. QEMU 机内测通过:mount 建表、lookup 按名找到、read 读出内容、readdir 列出条目。
3. inode 操作用函数指针表(不用虚函数)、私有数据靠 fs_private;resolve 有边界判定;FDTable 从 3 起算;Ramdisk 是 static 保活。
4. ramdisk 只读、扁平目录(不拔成层级树或可写)。

做到这四条,内核就有了完整的 VFS 管线。但用户态还碰不到——下一关(Lab 027b)接上系统调用和 shell。
