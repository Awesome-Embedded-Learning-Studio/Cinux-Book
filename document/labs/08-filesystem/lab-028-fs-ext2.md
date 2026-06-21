---
title: Lab 028 · 磁盘上的真文件系统:ext2 只读驱动
---

# Lab 028 · 磁盘上的真文件系统:ext2 只读驱动

> 配套章节:[028 · 磁盘上的真文件系统:ext2 只读驱动](../../book/08-filesystem/028-fs-ext2.md)。这一关给你目标和约束,不贴 inode 定位数学、不贴单间接块翻译、不贴目录项 rec_len 步进。前提是 Lab 025(AHCI 块读)和 Lab 027(VFS 接口)已就绪。

## 实验目标

在 AHCI 磁盘上实现一个 ext2 只读文件系统,实现 027 的 `FileSystem` 接口、挂进 VFS。拆成五个子目标:

1. 认 ext2 磁盘布局:超块、块组描述符表、inode、变长目录项。
2. 实现「读一块」的原子操作(经 AHCI 的 DMA 读)。
3. 挂载:读超块、验 magic、算参数、读 BGDT、拿到根 inode。
4. 按 inode 号定位 inode(块组 + 组内索引的数学)。
5. 找文件(逐分量遍历 + 目录项扫描)+ 读内容(直接块、单间接块、稀疏空洞)。

做完这五条,内核就能从磁盘读出 ext2 文件,ramdisk 被正式取代。注意:这一关只读,写是下一关(028b)。

## 前置条件

过 Lab 025:`ahci.read(port, lba, sectors, buf_phys)` 能用——这一关所有块 I/O 都靠它。

过 Lab 027:`FileSystem`(mount/lookup)、`Inode` + `InodeOps`、`vfs_mount_add` 都就绪——这一关写一个实现这套接口的 ext2 类挂上去。

要对 ext2 格式有基本了解:块组、inode 表、`i_block[15]` 的直接/间接索引、变长目录项。不懂先看 ext2 规范或 OSDev,别照着本关盲写。

## 任务分解

**第一步:磁盘结构。** 照 ext2 规范定几个 `[[gnu::packed]]` 结构:超块(1024B,关键 magic 0xEF53、log_block_size、blocks/inodes_per_group、各 count)、块组描述符(`bg_inode_table` 最关键)、inode(`i_mode` 类型位、`i_size`、`i_block[15]`)、目录项(inode/rec_len/name_len/file_type/name)。每个配 `static_assert` 焊住大小——磁盘结构差一个字节全错。

**第二步:读一块。** 先懒分配一个一页的 DMA 缓冲(PMM 要页、VMM 映射、清零)。然后 `read_block(block_num)`:把块号换算成 LBA(一块 = `block_size/512` 个扇区),调 `ahci.read` 把这块 DMA 进缓冲。想清楚「整个 ext2 共用一个缓冲、一次一块」意味着什么——任何读都会覆盖上一次的结果。

**第三步:挂载。** 读超块(字节偏移 1024 = LBA 2,2 扇区),验 magic,算出 block_size(左移)、sectors_per_block、inode_size、inodes_per_group、blocks_per_group、group_count。读 BGDT——想清楚它从哪个块开始(1KB 块时超块独占块1、BGDT 在块 2;更大块时 BGDT 在块 1)。最后读根 inode(inode 号 **2**)、放进缓存。

**第四步:inode 定位。** 给一个 inode 号,算它在磁盘哪:group = (ino-1)/inodes_per_group(想清楚为什么 -1)、index = (ino-1)%inodes_per_group、起始块 = bgdt[group].bg_inode_table、byte_offset = index*inode_size、再除 block_size 取块号和块内偏移。读那块、抠出 inode。这道数学算错全崩,务必和规范逐一对照。

**第五步:找文件 + 读内容。** lookup 做逐分量遍历:从根(ino 2)开始,每个分量调 `lookup_in_dir` 在当前目录的数据块里扫目录项(按 `rec_len` 步进、匹配 name_len+名字、返回 inode 号),中间分量必须是目录,最后一级返回缓存 inode。读内容(ext2_file_read):按 offset 算 file_block,前 12 块直接查 `i_block[]`,第 13 块起读单间接块(`i_block[12]`)、从中取第 idx 项(想清楚单缓冲约束——取完才能再读下一块),`disk_block==0` 是稀疏洞、填零。读完挂进 VFS:`vfs_mount_add("/", &ext2)`。

## 接口约束

你要实现出来的东西,对外长这样(职责和签名,不给实现):

- `Ext2 : FileSystem`(构造接 AHCI 引用 + port 号);`mount()`、`lookup(path)`。
- `read_block(block_num)`、`read_disk_inode(ino, out)`、`get_cached_inode(ino)`、`lookup_in_dir(dir_ino, name, len)`。
- InodeOps:`ext2_file_read`(直接 + 单间接 + sparse)、`ext2_dir_readdir`(`.`/`..` + 扫目录块)。

硬约束:

- **只读**:没有 create/write;ext2_file_read 只读。
- 间接块只实现**直接 + 单间接**;双间接/三间接跳过(小盘用不到,但别拔成支持大文件)。
- **单 DMA 缓冲**:整个 ext2 共用一个一页缓冲,一次一块,用完即覆盖;不预读、不缓存块。
- InodeOps 靠**全局 instance 指针**找回 Ext2(同时只能挂一个 ext2);inode 缓存是 64 槽、线性搜、FIFO 驱逐、slot 0 锁根。
- ext2 盘在 AHCI port **1**(port 0 是启动盘);`cmake/qemu.cmake` 里挂的,驱动得对应。

ext2 各结构字段、块大小换算、inode 数学、目录项步进,都得你照规范定,这关不提供。

## 验证步骤

纯算术(块大小换算、inode→group/index、file_block→disk_block 翻译、目录项 rec_len 步进)在 host 上镜像测:

```bash
ctest --test-dir build -R ext2 --output-on-failure
```

真盘在 QEMU 里验。先造盘:`mkfs.ext2 -b 1024 -O none -N 128` 造个 4MB ext2,用 `debugfs` 塞 `/etc/motd`、`/hello.txt`(脚本 `scripts/create_ext2_disk.sh` 已备)。QEMU 挂到 AHCI port 1。机内测验 mount 成功、lookup 找到、read 读出内容:

```bash
cmake --build build --target run-kernel-test
```

或直接跑完整内核,看 `[EXT2] Superblock valid: magic=0xef53`、`[VFS] ext2 mounted at /`,以及能读到 `/hello.txt` 内容。这几样出来,整条 AHCI→ext2→VFS 链路就通了。

## 常见故障

- **mount 时 magic 无效 / 根 inode 模式全错**:要么盘没在 port 1(读到了启动盘),要么超块偏移/LBA 算错。超块固定在字节 1024(=LBA 2)。
- **读单间接块拿到垃圾块号**:违反单缓冲约束——读间接块后没立刻取目标块号,中间又 read 了别的块,缓冲被覆盖。每次 read_block 后立刻取走需要的信息。
- **lookup 一钻目录就崩 / 认不出文件**:inode 定位数学错(`ino-1` 漏了、`bg_inode_table` 当字节算了、除法取余反了)。逐一和规范对照。
- **列目录死循环**:目录项 `rec_len==0` 没防,`pos+=0` 原地踏步。加 `if (rec_len==0) break;`,并防块尾半个目录项越界。
- **挂第二个 ext2 后前一个的文件读出乱码**:全局 instance 指针被覆盖。这套 InodeOps 设计同时只能挂一个 ext2——别假装能多挂。
- **大文件读不全**:文件超了 12 直接块 + 单间接的范围(双间接没实现)。小盘够用,但要知道这是边界。

## 通过标准

1. host 算术测全绿:block_size 换算、inode→group/index 数学、直接/单间接块翻译、目录项 rec_len 步进。
2. QEMU 机内测通过:mount(magic 有效、参数算对、BGDT 读到)、lookup 找到文件、read 读出内容。
3. 只读;间接块实现直接+单间接;单 DMA 缓冲且不跨 read 持有;inode 数学 1-based 且和规范一致;目录项防 rec_len==0。
4. 盘挂 AHCI port 1;挂进 VFS 后 027b 的 cat/ls 不改就能读 ext2。

做到这四条,内核就有了磁盘上能读的真文件系统。但还只读——下一关(028b)给 ext2 加写(分配 inode/块、写目录项、更新位图)。
