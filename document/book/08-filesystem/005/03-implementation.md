---
title: 03 · 实现:布局、DMA、mount、inode 定位、找文件、读内容
---

# 实现:布局、DMA、mount、inode 定位、找文件、读内容

### ext2 在磁盘上长什么样:超块、块组、inode、目录项

ext2 的磁盘布局是 [ext2_types.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ext2_types.hpp) 里那几个 `[[gnu::packed]]` 结构。每个都按规范定长、靠 `static_assert` 焊住。这里只点关键:

超块(`Ext2Superblock`)在**字节偏移 1024**(即 LBA 2),1024 字节。它是整个文件系统的「身份证」:magic 必须是 `0xEF53`;`s_log_block_size` 决定块大小(`block_size = 1024 << s_log_block_size`,所以 0→1KB、2→4KB);`s_inodes_per_group`、`s_blocks_per_group` 决定块组大小;`s_inodes_count`、`s_blocks_count` 是总量。挂载时这些都得读出来。

块组描述符(`Ext2BlockGroupDescriptor`)排成一张表(BGDT),紧跟超块所在块之后。每组一个,里面最关键字段是 `bg_inode_table`——这一组的 inode 表从哪个块开始。要定位一个 inode,先算它在哪组、再查这组的 `bg_inode_table`。

inode(`Ext2Inode`)是每个文件的元信息:`i_mode`(类型位,`S_IFREG=0x8000` 文件、`S_IFDIR=0x4000` 目录)、`i_size`(文件大小)、以及 `i_block[15]`——这是数据块指针数组,ext2 的核心:

```text
i_block[0..11]  直接块指针(12 个,每个指向一个数据块)
i_block[12]     单间接块指针(指向一个「装满块指针」的块)
i_block[13]     双间接块指针(指向「装满指向间接块的指针」的块)
i_block[14]     三间接块指针
```

小文件只用前 12 个直接块;大了用单间接(一块能装 `block_size/4` 个指针);再大用双间接、三间接。Cinux 这一章只实现**直接块 + 单间接**(双间接跳过、三间接不处理——4MB 测试盘用不到,注释里明说了)。这种「直接 + 多级间接」的索引,是 ext2(和很多老 FS)放大文件大小的标准手法。

目录项(`Ext2DirEntry`)是**变长**记录:inode 号、`rec_len`(这一条记录多长,4 字节对齐)、`name_len`、`file_type`、然后是名字。目录的数据块里就是一串这样的记录、靠 `rec_len` 一个接一个排。变长是为了删除时只标记、不搬移(把 `inode` 置 0、`rec_len` 并到前一条)。遍历目录就是「从块头开始,按 `rec_len` 步进,直到块尾」。

### 读一块的原子操作:单 DMA 缓冲 + AHCI

所有磁盘访问最终都归约到「读一个块」。ext2 用一个**共享的单块缓冲**,见 [ext2.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ext2.hpp) 里 `Ext2` 类的 `read_block`(本 tag 的实现集中在单个 `ext2.cpp`,后续 tag 才拆成 `ext2_block/inode/directory` 等多文件):

```cpp
bool Ext2::read_block(uint32_t block_num) {
    if (!ensure_dma_buffer()) return false;
    uint64_t lba = static_cast<uint64_t>(block_num) * sectors_per_block_;
    return ahci_.read(port_index_, lba,
                      static_cast<uint16_t>(sectors_per_block_),
                      dma_buf_phys_);   // 数据落在 dma_buf_phys_ 这页
}
```

`ensure_dma_buffer` 懒分配:第一次用时从 PMM 要一页、用 VMM 映射到固定的 `EXT2_DMA_VIRT_BASE`、清零。之后每次 `read_block` 都把目标块 DMA 进**这同一页**,内核再用 `dma_buf_virt_` 读它。`block_num`(ext2 块号)换算成 LBA:一块 = `sectors_per_block_` 个扇区(块 1KB = 2 扇区,4KB = 8 扇区)。然后调 001 的 `ahci.read(port, lba, sectors, phys)`——这一章的块 I/O 全靠它。

这个「单缓冲」设计有个必须时刻记住的约束:**缓冲里一次只有一块**。你读块 A 拿到一个指针表,然后读块 B,块 A 的内容就没了。所以单间接块的读法是「读间接块 → 立刻从缓冲里取出目标块号 → 再读目标块」,中间不能假定缓冲还留着间接块。调试现场会专门讲这个。

### mount:读超块、算参数、读 BGDT、拿到根 inode

`mount` 是标准的「读元数据、验明正身、算参数」流程:

```cpp
bool Ext2::mount() {
    ensure_dma_buffer();
    // ① 读超块:offset 1024 = LBA 2,2 个扇区
    ahci_.read(port_index_, 2, 2, dma_buf_phys_);
    memcpy(&sb_, reinterpret_cast<const uint8_t*>(dma_buf_virt_), sizeof(sb_));
    // ② 验 magic
    if (sb_.s_magic != EXT2_SUPER_MAGIC) return false;
    // ③ 算参数
    block_size_       = 1024U << sb_.s_log_block_size;
    sectors_per_block_ = block_size_ / EXT2_SECTOR_SIZE;
    inode_size_       = (sb_.s_rev_level == 0) ? 128 : sb_.s_inode_size;
    inodes_per_group_ = sb_.s_inodes_per_group;
    blocks_per_group_ = sb_.s_blocks_per_group;
    group_count_      = (sb_.s_blocks_count + blocks_per_group_ - 1) / blocks_per_group_;
    // ④ 读 BGDT(块组描述符表):紧跟超块所在块之后
    uint32_t bgdt_block = (block_size_ == 1024) ? 2 : 1;   // 1KB 块时超块在块1,BGDT在块2
    for (逐个 BGDT 块) { read_block(bgdt_block + i); memcpy 到 bgdt_[]; }
    // ⑤ 读根 inode(inode 2),放进缓存 slot 0
    Ext2Inode root; read_disk_inode(2, root);
    inode_cache_[0] = {2, root, ...}; populate_vfs_inode(inode_cache_[0]);
    root_inode_ = inode_cache_[0].vfs_inode;
    return true;
}
```

几个要点。超块固定在字节 1024,跟块大小无关(这是 ext2 规范的硬约定)。`block_size` 是左移算出来的(不是直接读),`0→1KB、1→2KB、2→4KB`。BGDT 起始块取决于块大小:1KB 块时超块独占块 1、BGDT 从块 2 开始;更大的块时超块在块 0 的 1024 偏移处、BGDT 从块 1 开始。根目录的 inode 号是 **2**(inode 1 保留),这是 ext2 规范定的。读完这些,ext2 就「挂上来」了。

### 从 inode 号定位 inode:块组 + 索引的数学

拿到一个 inode 号,怎么在磁盘上找到它?`read_disk_inode` 是一道纯数学,但算错就全崩:

```cpp
bool Ext2::read_disk_inode(uint32_t ino, Ext2Inode& out) {
    uint32_t group          = (ino - 1) / inodes_per_group_;          // 第几组(inode 号 1-based)
    uint32_t index_in_group = (ino - 1) % inodes_per_group_;          // 组内第几个
    uint32_t inode_table_block = bgdt_[group].bg_inode_table;         // 这组 inode 表起始块
    uint64_t byte_offset    = static_cast<uint64_t>(index_in_group) * inode_size_;
    uint32_t block_offset   = byte_offset / block_size_;              // 表内第几块
    uint32_t within_block   = byte_offset % block_size_;              // 块内偏移
    read_block(inode_table_block + block_offset);
    memcpy(&out, dma_buf_virt_ + within_block, sizeof(Ext2Inode));
}
```

三个关键约定:inode 号是 **1-based**(所以 `ino-1`);`bg_inode_table` 给的是这组 inode 表的**起始块**;一个 inode 可能横跨块边界(`within_block + sizeof(Inode) > block_size`),代码里做了边界检查拒绝跨块(实际 inode 128B、块 ≥1KB,基本不跨,但防一手)。这道数学是 ext2 的「地址翻译」核心,host 单测专门验它。

inode 读出来后,要么直接用(如 lookup 中间步骤),要么进缓存。`get_cached_inode` 维护一个 64 槽的 inode 缓存:命中直接返回、未命中读盘并填、满了按简单 FIFO 驱逐(slot 0 永远留给根)。`populate_vfs_inode` 把磁盘 inode 翻译成 003 的 VFS inode:按 `i_mode` 的类型位决定 `InodeType`、挂上对应的 InodeOps(目录挂 `ext2_dir_ops`、文件挂 `ext2_file_ops`),`fs_private` 指回缓存条目(这样 InodeOps 回调能找回磁盘 inode)。这个缓存很简陋——线性搜、FIFO 驱逐,**不是** Linux 的 inode/dentry cache,别拔高。

### 找文件:逐分量遍历 + 目录项扫描

`lookup(path)` 做的是「把 `/etc/motd` 这种路径,逐级钻进目录」:

```cpp
Inode* Ext2::lookup(const char* path) {
    if (根路径) return &root_inode_;
    if (path[0]=='/') ++path;
    uint32_t cur = 2;                       // 从根(inode 2)开始
    while (path 还有分量) {
        取出当前分量(comp_len,到下一个 '/' 或结尾)
        uint32_t found = lookup_in_dir(cur, path, comp_len);   // 在 cur 这个目录里找分量
        if (found == 0) return nullptr;     // 这一级找不到
        if (后面还有分量 && found 不是目录) return nullptr;    // 中间分量必须是目录
        cur = found;  path 跳过这个分量
    }
    return get_cached_inode(cur);           // 最后一级 → 返回缓存 inode
}
```

这和 003 ramdisk 的「扁平 lookup」完全不同——ext2 的 lookup 是**真正的多级目录遍历**,每钻一级调一次 `lookup_in_dir`。`lookup_in_dir` 在一个目录的数据块里扫目录项找名字:

```cpp
uint32_t Ext2::lookup_in_dir(uint32_t dir_ino, const char* name, uint32_t name_len) {
    Ext2Inode dir; read_disk_inode(dir_ino, dir);
    for (目录的每个数据块 blk = dir.i_block[b]) {
        read_block(blk);
        uint32_t pos = 0;
        while (pos < block_size) {
            entry = dma_buf_virt_ + pos;
            if (entry->rec_len == 0) break;                    // ★ 防 rec_len==0 死循环
            if (entry->inode != 0 && entry->name_len == name_len && 名字全等)
                return entry->inode;                            // 命中,返回 inode 号
            pos += entry->rec_len;                              // 步进到下一条
        }
    }
    return 0;   // 没找到
}
```

目录项是变长的,靠 `rec_len` 一步步往前挪。那个 `rec_len == 0` 的检查不能少——理论上规范的目录项 `rec_len` 不会是 0,但读到损坏数据或算错偏移时,`pos += 0` 会原地踏步、死循环。这是处理变长记录的标准防御。

### 读文件内容:直接块、单间接块、稀疏空洞

最后是 `ext2_file_read`(InodeOps),把 inode 指向的数据块读出来。它要处理「这个字节落在哪个块、那个块在磁盘哪」:

```cpp
int64_t ext2_file_read(const Inode* inode, uint64_t offset, void* buf, uint64_t count) {
    auto* cached = static_cast<const Ext2CachedInode*>(inode->fs_private);
    const Ext2Inode& disk = cached->disk_inode;
    // 按 i_size 截断
    uint64_t to_read = min(count, disk.i_size - offset);
    while (已读 < to_read) {
        uint64_t file_block = (offset + 已读) / block_size;     // 这个字节落在文件第几块
        uint32_t disk_block = 0;
        if (file_block < 12) {
            disk_block = disk.i_block[file_block];              // 直接块
        } else {
            uint32_t idx = file_block - 12;
            uint32_t indirect = disk.i_block[12];               // 单间接块
            read_block(indirect);                                // 读间接块(进 DMA 缓冲)
            disk_block = ((uint32_t*)dma_buf_virt())[idx];      // 立刻从缓冲取目标块号
        }
        if (disk_block == 0) { 填零; continue; }                // sparse 空洞
        read_block(disk_block);                                  // 读真正的数据块
        memcpy(buf + 已读, dma_buf_virt_ + 块内偏移, chunk);
    }
}
```

三个细节。第一,**file_block → disk_block 的翻译**:前 12 块直接查 `i_block[]`;第 13 块起要走单间接——先读 `i_block[12]` 这块(它本身是一堆块指针),从中取第 `idx` 项才是真正的数据块号。这正是「单缓冲」约束发挥作用的地方:读间接块拿到指针表后,必须**立刻**取出目标块号(因为紧接着的 `read_block(disk_block)` 会覆盖缓冲)。第二,**稀疏文件**:`disk_block == 0` 表示这块是「洞」(文件里没分配的块),ext2 规定读洞返回零,代码里老老实实填零。第三,双间接、三间接**没实现**(注释明说「small disks」)——4MB 测试盘上的小文件用不到,但这是个诚实的能力边界,别拔成支持大文件。

`ext2_dir_readdir`(列目录的 InodeOps)逻辑类似:index 0/1 返回 `.`/`..`,之后扫目录数据块、按 `rec_len` 步进数到第 `index-2` 个真实条目,把名字拷出来——和 003 ramdisk 的 readdir 同一套「靠 offset 当下标、一条条吐」的接口。

## 调试现场
