---
title: 02 · 设计图:ext2 在磁盘上的布局
---

# 设计图:ext2 在磁盘上的布局

## 设计图

ext2 把磁盘切成「块组」,每组有自己的 inode 表和数据块。读一个文件,是「定位 inode → 读它的数据块」;定位 inode 靠「块组 + 组内索引」;找一个名字,靠扫目录的数据块里的目录项。

```text
   磁盘(ext2,block_size=1024):
   ┌──────────┬──────────┬────────────────────┬───────────────────┐
   │ 引导块    │ 超块+BGDT │ 块组0的位图+inode表  │ 块组0的数据块 ...   │
   │ (1块)    │(块1/2起) │  (bg_inode_table)   │                   │
   └──────────┴──────────┴────────────────────┴───────────────────┘
   超块:offset 1024,magic 0xEF53,s_log_block_size(→block_size=1024<<n)、
         s_blocks_per_group、s_inodes_per_group、s_inodes_count...
   BGDT[i]:bg_inode_table = 第 i 组 inode 表的起始块号

   读一个块:read_block(n)
        lba = n * sectors_per_block
        ahci.read(port, lba, sectors_per_block, dma_buf_phys)   ← 数据进【单个共享 DMA 缓冲】
        (整个 ext2 共用一个一页缓冲,一次一块,用完即覆盖)

   inode 号 → inode(以 ino=2 根为例):
        group      = (ino-1) / inodes_per_group
        index      = (ino-1) % inodes_per_group
        inode块    = bgdt[group].bg_inode_table + (index*inode_size)/block_size
        块内偏移   = (index*inode_size) % block_size
        read_block(inode块) → 从 dma_buf 抠出 Ext2Inode

   找文件 /etc/motd(逐分量):
        cur = 2(根)
        "etc"  → lookup_in_dir(cur,"etc")  → 扫根目录数据块的目录项 → 命中 → 新 ino
        "motd" → lookup_in_dir(etc_ino,"motd") → 扫 etc 目录 → 命中 → 最终 ino
        get_cached_inode(最终ino) → Inode*

   读文件内容(ext2_file_read):
        file_block = offset / block_size
        if file_block < 12:  disk_block = inode.i_block[file_block]        ← 直接块
        else:                读 i_block[12](单间接块)→ 取其中第 idx 项      ← 单间接
        disk_block==0:       sparse 空洞,填零
        read_block(disk_block) → 从 dma_buf 拷 chunk 到用户 buf
```

一条贯穿全局的设计:**整个 ext2 只用一个一页大的 DMA 缓冲**。每次 `read_block` 都把一个块读进这同一个缓冲,读完立刻用(拷走或解析),下一次读覆盖它。这是个极简方案——不预读、不缓存块、不流水线,一次只读一块。简单,但代价是任何中间结果(比如读单间接块拿到的指针表)用完就得拷走,不能指望它还在缓冲里(下一轮 read 会冲掉)。

## 代码路线
