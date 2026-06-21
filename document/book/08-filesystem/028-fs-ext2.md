---
title: 028 · 磁盘上的真文件系统:ext2 只读驱动
---

# 028 · 磁盘上的真文件系统:ext2 只读驱动

> 026/027 的 ramdisk 证明「文件抽象」成立,但它只读、数据还是构建期嵌进内核的——不算真正的文件系统。这一章我们上真家伙:在 025 那块 AHCI 磁盘上,实现一个 ext2 文件系统驱动。它从磁盘读超块、块组描述符、inode、目录项,能按路径找到文件、读出内容。完成后,内核挂载的就是一块「磁盘上、按 ext2 标准布局」的真文件系统,ramdisk 这条线正式被它取代。先说边界:这一章的 ext2 是**只读**的(写是 028b 的事),而且因为 ext2 是个有标准的成熟格式,我们重点讲「Cinux 怎么把它读出来」,格式的每个字段细节会引规范、不逐一铺开。

## 这一章我们要点亮什么

把 027 定义的 `FileSystem` 接口,用一个**真实磁盘文件系统**实现出来。四件事:

第一,认 ext2 的**磁盘布局**:超块(全局元数据)、块组描述符表(每组一块的索引)、inode(每个文件的元信息+数据块指针)、目录项(目录里「名字 → inode 号」的变长记录)。

第二,从 AHCI 盘上把这些结构**读进内存**:每次读一个块,经 025 的 `ahci.read` 走 DMA。

第三,**挂载**:读超块、验 magic、算出块大小/块组数等参数、读块组描述符表、拿到根目录(inode 2),把 ext2 注册进 027 的 VFS 挂载表。

第四,**找文件 + 读内容**:`lookup` 按路径逐级钻进目录、`ext2_file_read` 通过 inode 的数据块指针(直接块、单间接块)把文件内容读出来。

验收点是启动时的一串 `[EXT2]` 日志(magic 有效、块大小、块组数),以及把 ext2 挂到 `/` 后,通过 VFS 能读出磁盘上 `/hello.txt` 的内容。能从磁盘读出文件,这一章就成了。

## 为什么现在需要它

为什么紧跟 027。027 把 VFS 的骨架搭好了(inode、FileSystem 接口、挂载表),但唯一实现这个接口的后端是 ramdisk——一个只读、数据嵌在内核镜像里的「玩具」文件系统。要证明 VFS 这套抽象真有用,得有个**正经后端**实现它:数据在磁盘上、按公认格式布局、能被标准的 `mkfs` 工具创建。ext2 就是这个后端。这一章让 027 的 VFS 第一次接上真实存储。

为什么是 ext2 而不是别的。ext2 是经典、文档齐全、格式相对简单(没有日志、没有 B 树目录),又足够「真」(Linux 早期就用它、`mkfs.ext2` 现成)。它正好是教学文件系统的甜点:既有真实文件系统的全部要素(块组、inode、间接块、变长目录项),又没有 ext3/ext4 的复杂度(日志、extent、HTree)。我们读它,等于把「文件系统怎么工作」这件事走一遍标准流程。

还有一笔关于「复用」的账。这一章几乎不发明新东西:块 I/O 走 025 的 AHCI、文件对象和操作表用 027 的 inode + InodeOps、挂载走 027 的 `vfs_mount_add`。ext2 这个类要做的,就是实现 027 那两个虚方法(`mount`/`lookup`)和几个 InodeOps 函数。这正好验证 027 的设计目标——「换个后端,上层一行不改」。实际上 main.cpp 里把 `vfs_mount_add("/", &ramdisk)` 换成 `vfs_mount_add("/", &ext2)`,027b 的 `cat`/`ls` 立刻就能读 ext2 盘上的文件了。

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

`ensure_dma_buffer` 懒分配:第一次用时从 PMM 要一页、用 VMM 映射到固定的 `EXT2_DMA_VIRT_BASE`、清零。之后每次 `read_block` 都把目标块 DMA 进**这同一页**,内核再用 `dma_buf_virt_` 读它。`block_num`(ext2 块号)换算成 LBA:一块 = `sectors_per_block_` 个扇区(块 1KB = 2 扇区,4KB = 8 扇区)。然后调 025 的 `ahci.read(port, lba, sectors, phys)`——这一章的块 I/O 全靠它。

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

inode 读出来后,要么直接用(如 lookup 中间步骤),要么进缓存。`get_cached_inode` 维护一个 64 槽的 inode 缓存:命中直接返回、未命中读盘并填、满了按简单 FIFO 驱逐(slot 0 永远留给根)。`populate_vfs_inode` 把磁盘 inode 翻译成 027 的 VFS inode:按 `i_mode` 的类型位决定 `InodeType`、挂上对应的 InodeOps(目录挂 `ext2_dir_ops`、文件挂 `ext2_file_ops`),`fs_private` 指回缓存条目(这样 InodeOps 回调能找回磁盘 inode)。这个缓存很简陋——线性搜、FIFO 驱逐,**不是** Linux 的 inode/dentry cache,别拔高。

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

这和 027 ramdisk 的「扁平 lookup」完全不同——ext2 的 lookup 是**真正的多级目录遍历**,每钻一级调一次 `lookup_in_dir`。`lookup_in_dir` 在一个目录的数据块里扫目录项找名字:

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

`ext2_dir_readdir`(列目录的 InodeOps)逻辑类似:index 0/1 返回 `.`/`..`,之后扫目录数据块、按 `rec_len` 步进数到第 `index-2` 个真实条目,把名字拷出来——和 027 ramdisk 的 readdir 同一套「靠 offset 当下标、一条条吐」的接口。

## 调试现场

028 本身没有 notes(pack 里那两条 GP fault / MMIO 碰撞是 028b/028e 的,属后续 tag,不进本章)。但 ext2 这套有几个高发坑,值得当调试现场。

一是 **单 DMA 缓冲被覆盖**。整个 ext2 共用一个一页缓冲。读单间接块时,缓冲里是指针表;紧接着读数据块,指针表就被冲掉了。如果你写代码时把「读间接块」和「读数据块」之间还插了别的 read,或者把指针表地址存下来想稍后用——指望着缓冲内容还在——就拿到了被覆盖后的垃圾。规矩:每次 read_block 后立刻把需要的信息取走(拷贝或解析),绝不跨下一次 read 持有缓冲指针。

二是 **inode 定位数学算错**。`(ino-1)` 的 1-based、`bg_inode_table` 是块号不是字节、`index*inode_size` 的除法取块/取余——任何一个搞反,读出来的 inode 是错的。症状是 mount 时根 inode 读出来 mode 全错、magic 验过了但根目录认不出,或 lookup 一钻目录就崩。这道数学必须和规范逐一对照,host 单测专门焊它。

三是 **`rec_len == 0` 导致死循环**。扫目录项靠 `pos += rec_len` 步进。损坏数据或偏移算错时遇到 `rec_len==0`,pos 不前进、原地踏步死循环。那道 `if (entry->rec_len == 0) break;` 不能省。同样,`pos + 头大小 > block_size` 的越界检查也得有,防止读到块尾半个目录项。

四是 **全局 `g_ext2_instance` 限制单挂载**。InodeOps 的函数签名里没有 `Ext2*`,回调里要读块只能靠一个全局 `g_ext2_instance` 找回实例。这意味着同时**只能挂一个 ext2**——挂第二个会覆盖这个全局,前一个的 InodeOps 回调就指错了实例。这是 027 那套「InodeOps 是自由函数」设计的代价(027 的吐槽里说了,这套后来会被重构成虚函数、实例随 inode 走,这个全局也就没必要了)。这一章老实接受「单 ext2 挂载」的限制。

五是 **AHCI port 选错**。ext2 盘在 port **1**(port 0 是启动/内核盘)。`Ext2 ext2(ahci, 1)` 写成 port 0,会去读启动盘、读到一堆非 ext2 数据,magic 校验直接失败(`Invalid magic`)。盘在哪个 port 是 `cmake/qemu.cmake` 里挂的,驱动得对应。

## 验证

ext2 的逻辑(块大小换算、inode→块组/索引数学、直接/间接块翻译、目录项 rec_len 步进)大半能在 host 上镜像测。[test_ext2.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_ext2.cpp) 把这些纯算术抄了一份测:`block_size` 从 `log_block_size` 算对、inode 号到 group/index 的除法取余、直接块 vs 单间接块的 file_block→disk_block 翻译、目录项按 rec_len 步进:

```bash
ctest --test-dir build -R ext2 --output-on-failure
```

真盘、真 AHCI、真 ext2 在 QEMU 里验。先造盘——[create_ext2_disk.sh](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/scripts/create_ext2_disk.sh) 用 `mkfs.ext2 -b 1024 -O none -N 128` 造个 4MB ext2,再用 `debugfs` 往里塞 `/etc/motd` 和 `/hello.txt`(用 debugfs 是为了不用 root/挂载权限)。QEMU 把它挂到 AHCI port 1。机内测 [test_ext2.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_ext2.cpp) 验 mount 成功、按路径 lookup 找到文件、read 读出内容:

```bash
cmake --build build --target run-kernel-test
```

或直接跑完整内核,验收点是启动时那段 `[EXT2]` 日志——`Superblock valid: magic=0xef53`、`block_size=`、`groups=`,以及 `[VFS] ext2 mounted at /`。能读到磁盘上 `/hello.txt` 的内容(`Hello from ext2!`),整条「AHCI 读块 → ext2 解析 → VFS 暴露」链路就通了。这章的难点和前几章一样:正确性靠现象间接验证,所以 host 算术测(焊死 inode/块翻译)+ 机内测(真盘跑一遍)缺一不可。

## 下一站

到这里,内核挂载的是一块**磁盘上、按 ext2 标准布局、能读**的真文件系统了——026/027 的 ramdisk 被它取代,027b 的 `cat`/`ls` 不用改一行就能读 ext2 盘。但这个 ext2 是**只读**的:`ext2_file_read` 能读,却没有 `create`/`write`——你不能在 shell 里 `touch` 一个文件或写点什么。

下一站(028b),给 ext2 加上**写**:分配新 inode、分配数据块、写目录项、更新位图和统计。有了写,ext2 才算个「能用的」文件系统,而不是只读快照。再往后(028c 及之后)还有 `cwd`/`stat`、同步安全、init 线程模型等一串打磨。不过那是后面的事,我们先把「能从磁盘读 ext2 文件」这个里程碑坐实。

---

### 参考

- ext2 规范 — *The Second Extended Filesystem*([ext2_types.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ext2_types.hpp) 头注释已引):超块字段与 magic `0xEF53`、块组与块组描述符、inode 的 `i_block[15]`(12 直接 + 单/双/三间接)、inode 号 1-based、根目录 inode=2、变长目录项(`rec_len`/`name_len`/`file_type`)、稀疏文件。权威格式依据。社区速查可参考 [OSDev — ext2](https://wiki.osdev.org/Ext2)。
- 027 章 · [给文件一个统一接口:VFS 内核层](027-fs-vfs.md):ext2 实现的就是 027 定义的 `FileSystem`(mount/lookup)和 InodeOps,挂进同一套 VFS——本章是 027 抽象的第一个「真」后端。
- 025 章 · [让内核自己找到磁盘:PCI 枚举与 AHCI 驱动](025-driver-ahci.md):ext2 的所有块 I/O 都走 `ahci.read(port, lba, sectors, buf_phys)`。
- 本 tag 源码:[ext2_types.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ext2_types.hpp)、[ext2.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ext2.hpp)(本 tag 的 `Ext2` 实现集中在单个 `ext2.cpp`,后续 tag 才拆成 `ext2_block/inode/directory/init/common` 等多文件)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp)(`static Ext2 ext2(ahci, 1)` + 挂 `/`)、[create_ext2_disk.sh](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/scripts/create_ext2_disk.sh);测试 [test_ext2.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_ext2.cpp)(host 算术镜像)、[test_ext2.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_ext2.cpp)(QEMU 真盘)。
