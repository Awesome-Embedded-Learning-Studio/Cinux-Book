---
title: 028b · 让 ext2 能写:从只读到可建可删
---

# 028b · 让 ext2 能写:从只读到可建可删

> 028 那章我们把 ext2 的「读」走通了:挂载、定位 inode、扫目录、顺着数据块把文件内容读出来。但它只读——盘上的文件全是构建期用 `debugfs` 塞进去的静态数据,内核运行起来一个字都改不了。一个改不了的文件系统,严格说只验证了「读」这一半。这一章我们补上另一半:**让 ext2 能写、能建、能删,而且改动要真正落回 AHCI 磁盘**。补完之后,用户态敲一条 `echo hi > /hello.txt`,重启再读,`hi` 还在——这才是「活的」文件系统。
>
> 先把边界说清楚:028b 的写是**够用但朴素**的。它没有日志、没有事务、没有多缓冲,全程靠 028 那块**唯一的 DMA 缓冲**做「读—改—写」;文件写实际只覆盖到直接块加单间接块的头一个槽(大文件会被静默截断);时间戳全是 0,因为 Cinux 还没有实时时钟。这些不是疏漏,是这一章我们主动选择的最小可写集。我们会逐个说清楚为什么是这样、踩了什么坑。

## 这一章我们要点亮什么

在 028 的只读 ext2 之上,把「写」这一整套补齐。四件事。

第一,**写回基础设施**。028 只有 `read_block`,这一章加 `write_block`,以及围绕它的 `write_disk_inode`、`write_superblock`、`write_bgdt`。它们统一长一个样:先读、再改、再写回(read-modify-write)。为什么是这个姿势,是本章的主线。

第二,**两个分配器**。要建文件就得有地方放它的 inode 和数据块,所以要能从块位图里分一块、从 inode 位图里分一个 inode。`alloc_block`/`free_block` 和 `alloc_inode`/`free_inode` 干这件事。

第三,**建、删、写内容**。`create`(建文件)、`mkdir`(建目录)、`unlink`(删),以及 `Ext2FileOps::write`(往文件里写字节)。这几个把分配器和写回基础设施串成一条链。

第四,**接上用户态**。四个新系统调用 `sys_creat`/`sys_mkdir`/`sys_unlink`/`sys_rmdir`(号码复用 Linux 的 85/83/87/84),加上 shell 的 `touch`、`mkdir`、`rm`、`rmdir`,以及 `echo` 的 `>` 重定向。

验收点很直白:在 shell 里 `echo hi > /hello.txt`,然后把它读回来,看到 `hi`。能写进去、能读出来、还落了盘,这一章就成了。

## 为什么现在需要它

为什么紧跟 028。028 证明了一件事:Cinux 的 VFS 抽象(`FileSystem` 接口、`Inode`、挂载表)能接上一个按 ext2 标准布局的真磁盘文件系统。但只读意味着这套抽象只被「读」这一侧验证过。写的难度不在「写」这个动作本身,而在它牵出的一连串新问题:新文件放哪、新 inode 从哪来、目录项怎么插进去、改完哪些元数据要同步刷盘、中途失败了怎么回滚。把这些走通,VFS 这套抽象才算真正立住。

还有一笔关于「复用」的账,和 028 一样漂亮。这一章的写路径几乎不长在新发明上:块 I/O 还是走 025 的 AHCI(只是第一次真正用了它的 `write`)、文件对象和操作表还是 027 的 `Inode` + `InodeOps`、挂载还是 027 的 `vfs_mount_add`。我们做的事,是给 027 的 `InodeOps` 加上 `write`/`create`/`mkdir`/`unlink` 四个虚方法,再让 ext2 实现它们。`main.cpp` 一行没动——挂载还是 028 的 `static Ext2 ext2(ahci, 1)` 配 `vfs_mount_add("/", &ext2)`。028b 的全部新意,都在 ext2 内部和 syscall 层。

顺带说一句关于 AHCI 的。025 那章我们让 AHCI 能 `read`,028 的 ext2 只用它读。其实 AHCI 的 `write`(走 ATA `WRITE DMA EXT`,命令码 0x35)在 028 那波重构里就已经备好了,只是一直没人调。028b 是它第一次真正被用来落盘。所以本章不会讲「怎么给 AHCI 加写」,那早就有了;我们讲的是 ext2 怎么用它,以及用它写盘时要注意什么。

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

这是 028 那块「单 DMA 缓冲」(一次只装一块、用完即覆盖)的直接后果,也是本章所有写代码的底色。

## 代码路线

### 写回的统一姿势:read-modify-write 与那块唯一的 DMA 缓冲

先看最底下的 `write_block`,它是 `read_block` 的镜像:把 DMA 缓冲里的内容按块写回磁盘。

```cpp
bool Ext2::write_block(uint32_t block_num) {
    if (!ensure_dma_buffer()) return false;
    uint64_t lba = static_cast<uint64_t>(block_num) * sectors_per_block_;
    return ahci_.write(port_index_, lba,
                       static_cast<uint16_t>(sectors_per_block_),
                       dma_buf_phys_);
}
```

注意它写的是 `dma_buf_phys_`——那块缓冲的**物理**地址。AHCI 做的是 DMA,PRDT 里填的是物理地址,内核虚拟地址对控制器没意义。这和 `read_block` 是对称的(读也是 DMA 到同一块缓冲)。

`write_block` 本身平淡,真正有意思的是它的调用者怎么用它。看 `write_disk_inode`——把一个 inode 写回磁盘:

```cpp
bool Ext2::write_disk_inode(uint32_t ino, const Ext2Inode& inode) {
    // ... 算出这个 inode 落在哪个块、块内偏移 ...
    uint32_t target_block = inode_table_block + block_offset;

    // 读-改-写:先把整块读进 DMA 缓冲
    if (!read_block(target_block)) return false;

    // 在缓冲里把这一个 inode 的字节覆盖掉
    auto* block_data = reinterpret_cast<uint8_t*>(dma_buf_virt_);
    memcpy(block_data + within_block_offset, &inode, sizeof(Ext2Inode));

    // 把整块写回
    return write_block(target_block);
}
```

这就是「读—改—写」。为什么非得先读?因为一个 1024 字节的块里挤着好几个 inode(`1024/128 = 8` 个),你只想改其中一个,就得先把整块读出来、只覆盖那 128 字节、再把整块写回去。如果跳过 `read_block` 直接写,就会把同块里另外 7 个 inode 全擦成 0。

这条规律贯穿 028b 的所有元数据写:

- `write_disk_inode`:读 inode 表块 → 改一个 inode → 写回。
- `write_bgdt(group)`:读 BGDT 块 → 改一个组描述符(32 字节)→ 写回。
- 改位图(分配/释放块或 inode):读位图块 → 置位/清位 → 写回。

只有 `write_superblock` 是个例外——它不读,直接全写:

```cpp
bool Ext2::write_superblock() {
    constexpr uint64_t SB_LBA = EXT2_SUPERBLOCK_OFFSET / EXT2_SECTOR_SIZE; // = 2
    auto* dma = reinterpret_cast<uint8_t*>(dma_buf_virt_);
    memcpy(dma, &sb_, sizeof(Ext2Superblock));     // 整个超块搬进缓冲
    return ahci_.write(port_index_, SB_LBA, 2, dma_buf_phys_);  // 直接写 2 扇区
}
```

超块本身就是 1024 字节、占满它该在的地方,而且我们要写的就是它的全部字段(比如 `s_free_blocks_count`),所以不需要 read-modify-write,把内存里 `sb_` 整个倒进去就行。它甚至没走 `write_block`,而是直接按扇区写 LBA 2——因为超块的位置是按字节偏移 1024 固定的,不参与 ext2 的块号编址。

读—改—写有个隐含的硬约束,值得单独点出来:**整个 ext2 共用那一块 DMA 缓冲,而一次 read-modify-write 要用到它两次(读一次、写一次)**。这意味着在这些操作期间,中间不能插入任何别的块 I/O,否则缓冲内容会被覆盖,写回去的就是错的。028b 没有并发(单核、无抢占、syscall 同步执行),所以暂时相安无事;但这根弦要记着——将来要是加了中断驱动的异步 I/O 或预读,这块单缓冲就是第一个要拆掉的东西。

### 两个分配器:扫位图找空闲

要建文件,先得有 inode;要让文件有内容,先得有数据块。ext2 用两套位图管理这些资源:每个块组有一块**块位图**(每位对应一个数据块,1=已用、0=空闲)和一块 **inode 位图**(每位对应一个 inode)。两个分配器长得几乎一样,我们以 `alloc_block` 为例:

```cpp
uint32_t Ext2::alloc_block() {
    for (uint32_t group = 0; group < group_count_; ++group) {
        if (bgdt_[group].bg_free_blocks_count == 0) continue;   // 这组满了,跳过

        uint32_t bitmap_block = bgdt_[group].bg_block_bitmap;
        if (!read_block(bitmap_block)) return 0;                 // 读位图块
        auto* bitmap = reinterpret_cast<uint8_t*>(dma_buf_virt_);

        // 逐字节、逐位找一个 0
        for (uint32_t byte_idx = 0; byte_idx < bytes_needed; ++byte_idx) {
            if (bitmap[byte_idx] == 0xFF) continue;              // 这 8 位全满
            for (uint32_t bit = 0; bit < 8; ++bit) {
                if ((bitmap[byte_idx] & (1U << bit)) == 0) {     // 找到空闲位
                    bitmap[byte_idx] |= static_cast<uint8_t>(1U << bit);  // 置位
                    write_block(bitmap_block);                   // 写回位图块

                    // 同步计数:全局 + 本组
                    --sb_.s_free_blocks_count;
                    --bgdt_[group].bg_free_blocks_count;
                    write_superblock();
                    write_bgdt(group);

                    return first_block + byte_idx * 8 + bit;     // 全局块号
                }
            }
        }
    }
    return 0;  // 盘满了
```

逻辑很直白:挨个块组看,跳过没有空闲的;读它的位图,从头扫一个 0 位;置位、写回位图块。但**容易漏的是后面那三行写回**。置了位还不够,`s_free_blocks_count`(超块里的全局空闲块数)和 `bg_free_blocks_count`(本组描述符里的空闲块数)都得减一,而且这两个字段分别在超块和 BGDT 里,得分别 `write_superblock()` 和 `write_bgdt(group)` 刷回去。

为什么这么啰嗦?因为 ext2 的空闲计数是**冗余存储**的:全局总数在超块,每组的小计在各自的组描述符。两者必须一致。如果只改了位图、忘了同步计数,文件系统表面上还能用(位图才是分配的真正依据),但 `df` 之类的工具看到的空闲数就是错的,而且一旦哪天有代码信任这些计数做决策(比如「这组满了就跳过」),就会分错块。`free_block`、`alloc_inode`、`free_inode` 全都照着这个三件套(改位图 + 改超块计数 + 改 BGDT 计数)来,一个都不能少。

返回值 0 在 ext2 里是个「非法值」约定:块号和 inode 号都是 1-based(根目录 inode 是 2),0 表示「没有」。所以 `alloc_block`/`alloc_inode` 拿到 0 就是失败,调用者要回滚。

### 写文件:直接块、单间接的首块,以及那条会截断的循环

分配器有了,看怎么往文件里写字节。`Ext2FileOps::write` 逐块处理:

```cpp
int64_t Ext2FileOps::write(Inode* inode, uint64_t offset,
                           const void* buf, uint64_t count) {
    auto* cached = static_cast<Ext2CachedInode*>(inode->fs_private);
    Ext2Inode& disk = cached->disk_inode;
    uint32_t bs = ext2_.block_size();

    while (total_written < count) {
        uint64_t file_block = (offset + total_written) / bs;
        uint64_t block_offset = (offset + total_written) % bs;
        uint64_t chunk = min(bs - block_offset, count - total_written);

        if (file_block > EXT2_DIRECT_BLOCKS) break;          // ← 边界,见下文

        uint32_t disk_block = ext2_.get_or_alloc_block(disk, file_block);
        // ...
        if (block_offset != 0 || chunk != bs)                // 部分块:先读原块
            ext2_.read_block(disk_block);
        else                                                  // 整块:清零缓冲
            memset(dma_buf, 0, bs);

        memcpy(dma_buf + block_offset, src + total_written, chunk);  // 填入新数据
        ext2_.write_block(disk_block);                       // 写回
        total_written += chunk;
    }
    // 更新 i_size / i_blocks,write_disk_inode
}
```

这里有两件值得拆开讲的事。

第一,**部分块写又是 read-modify-write**。如果一次写没有正好对齐到块边界、也没写满一整块(`block_offset != 0 || chunk != bs`),就必须先 `read_block` 把这块原来的内容读出来,再覆盖掉中间那一段。否则会把同块里别的内容(比如文件原来的字节)擦掉。只有「整块写」(从块首写到块尾)才能跳过读、直接把缓冲清零再填——因为反正整块都要被新内容覆盖。这和 `write_disk_inode` 是同一个道理。

第二,**那条会截断的循环**。注意循环里这一句:

```cpp
if (file_block > EXT2_DIRECT_BLOCKS) break;
```

`EXT2_DIRECT_BLOCKS` 是 12。它的意思是:一旦要写的块号超过 12,就直接跳出循环、停止写入。也就是说,`Ext2FileOps::write` 实际只写到第 12 个逻辑块(块号 0..12,共 13 块)。按 block_size=1024 算,一个文件最多写约 13KB,超出的部分被**静默截断**——`write` 返回的是实际写入的字节数,比你要求的小,但不报错。

这里有个微妙的不对称,必须讲清楚。负责「取或分配某逻辑块对应的数据块」的 `get_or_alloc_block`,本身是支持单间接块的:

```cpp
uint32_t Ext2::get_or_alloc_block(Ext2Inode& disk, uint32_t file_block) {
    if (file_block < EXT2_DIRECT_BLOCKS) {            // 直接块 0..11
        // 没分配就 alloc_block,清零,记进 i_block[file_block]
    }
    if (file_block < EXT2_DIRECT_BLOCKS + block_size_/4) {  // 单间接块 12..267
        // 分配/读取间接块 i_block[12],在里面分配一个指针
    }
    return 0;  // 双间接/三间接:不支持
}
```

`get_or_alloc_block` 能处理直接块(0..11)和单间接块(12..267,一个间接块装 `1024/4 = 256` 个指针)。但 `Ext2FileOps::write` 的循环在 `file_block > 12` 时就 break 了,根本到不了单间接块的第 2 个槽(index 13)。换句话说,**单间接块那 256 个槽,写路径只用了第 1 个**。这是 028b 一个真实的、有意识的小局限:读(028)支持完整的单间接,写(028b)却卡在第 13 块。够 `echo` 写点配置、够测试验证写链路,但写不了大文件。我们没有把它伪装成「支持大文件」——`get_or_alloc_block` 留着单间接分支,是因为删除路径要用它(下一节),而不是因为写路径真的用到了。

### 建文件与建目录:create / mkdir 的资源管理与回滚

有了分配器和写回,`create`(建普通文件)就是按部就班地把它们串起来,难在**失败要回滚**:

```cpp
Inode* Ext2::create(uint32_t parent_ino, const char* name, uint32_t name_len) {
    if (lookup_in_dir(parent_ino, name, name_len) != 0) return nullptr;  // 查重

    Ext2Inode dir_disk;
    read_disk_inode(parent_ino, dir_disk);                  // 读父目录

    uint32_t new_ino = alloc_inode();                       // ① 分 inode
    if (new_ino == 0) return nullptr;

    Ext2Inode new_disk{};
    new_disk.i_mode = EXT2_S_IFREG | 0644;                  // 普通文件, rw-r--r--
    new_disk.i_links_count = 1;
    // ...其余字段清零/置默认...

    if (!write_disk_inode(new_ino, new_disk)) { free_inode(new_ino); return nullptr; }  // ② 写 inode,失败回滚

    if (!add_dir_entry(parent_ino, dir_disk, new_ino, name, name_len, Regular)) {
        free_inode(new_ino); return nullptr;                // ③ 加目录项,失败回滚
    }

    write_disk_inode(parent_ino, dir_disk);                 // ④ 父目录可能变了(i_size/i_block)
    return get_cached_inode(new_ino);
}
```

注意那条把 `new_disk` 清零的语句 `Ext2Inode new_disk{}`(源码里是一个逐字节清零循环)。它看起来无害,却正是后面「调试现场」里那个 GP fault 的触发点——编译器会把它优化成一条要求 16 字节对齐的 SSE 指令 `movaps`。先记着。

`mkdir` 比 `create` 多几步,因为目录天生需要一个数据块来放 `.` 和 `..`:

```cpp
Inode* Ext2::mkdir(uint32_t parent_ino, const char* name, uint32_t name_len) {
    // ...查重、读父目录、alloc_inode...
    uint32_t data_blk = alloc_block();                      // 目录要一个数据块
    if (data_blk == 0) { free_inode(new_ino); return nullptr; }

    new_disk.i_mode = EXT2_S_IFDIR | 0755;                  // 目录, rwxr-xr-x
    new_disk.i_size = block_size_;                          // 一个块那么大
    new_disk.i_links_count = 2;                             // "." + 父目录里的这一项
    new_disk.i_block[0] = data_blk;
    write_disk_inode(new_ino, new_disk);

    // 在新数据块里写 "." 和 ".."
    auto* dma = (uint8_t*)dma_buf_virt_;
    memset(dma, 0, block_size_);
    // "." : inode=new_ino, rec_len=12
    // ".." : inode=parent_ino, rec_len=剩下整块
    write_block(data_blk);

    add_dir_entry(parent_ino, dir_disk, new_ino, name, Directory);

    dir_disk.i_links_count++;                               // 父目录多了一个 ".." 指向它
    bgdt_[new_group].bg_used_dirs_count++; write_bgdt(new_group);  // 组的目录计数 +1
    write_disk_inode(parent_ino, dir_disk);
    // ...
}
```

这里有几个 ext2 目录语义的细节,理解了才知道为什么这么写:

- 新目录的 `i_links_count = 2`:一个来自它自己的 `.` 项(指向自己),一个来自父目录里刚加的那个目录项。这是 ext2 计算目录硬链接数的规矩。
- `..` 指向父目录,所以**父目录的 `i_links_count` 要 +1**(它又被一个 `..` 指向了)。
- `bg_used_dirs_count`:每个块组描述符里有个「本组目录数」字段,建目录时要 +1。ext2 的 `mkfs` 和 `fsck` 会用到它来平衡目录在组间的分布。

这些计数看着琐碎,但它们是 ext2 一致性的一部分。`mkdir` 漏掉任何一个,`fsck` 就会报错。

回滚的规矩也值得强调:每一步分配,都要为它可能的失败配一个释放。`alloc_inode` 后写 inode 失败 → `free_inode`;`alloc_block` 后任何步骤失败 → 先 `free_block` 再 `free_inode`。否则就会留下「分配了但没人引用」的孤儿 inode/块——位图说它占了,可没有任何目录项指向它。

### 删除:unlink 释放数据块(能删的,写不出)

`unlink` 负责删除。它的核心逻辑是:从父目录移除目录项,把目标 inode 的链接数减一;如果链接数归零,就把它的数据块和 inode 本身都还回去。

```cpp
int Ext2::unlink(uint32_t parent_ino, const char* name, uint32_t name_len) {
    Ext2Inode dir_disk;
    read_disk_inode(parent_ino, dir_disk);

    uint32_t entry_ino = 0;
    remove_dir_entry(parent_ino, dir_disk, name, name_len, entry_ino);  // 从父目录移除

    Ext2Inode target_disk;
    read_disk_inode(entry_ino, target_disk);
    if (target_disk.i_links_count > 0) target_disk.i_links_count--;

    if (target_disk.i_links_count == 0) {
        // 没人引用了:释放全部数据块
        for (uint32_t i = 0; i < EXT2_DIRECT_BLOCKS; ++i)              // 直接块 0..11
            if (target_disk.i_block[i] != 0) free_block(target_disk.i_block[i]);

        if (target_disk.i_block[EXT2_INDIRECT_BLOCK] != 0) {           // 单间接块
            uint32_t indirect_blk = target_disk.i_block[EXT2_INDIRECT_BLOCK];
            read_block(indirect_blk);
            auto* indirect = (uint32_t*)dma_buf_virt_;
            for (uint32_t i = 0; i < ptrs_per_block; ++i)              // 间接块指向的所有数据块
                if (indirect[i] != 0) free_block(indirect[i]);
            free_block(indirect_blk);                                  // 间接块本身
        }
        // 清 size/blocks,write_disk_inode,free_inode
        // 若是目录:bg_used_dirs_count--,父目录 links_count--
    } else {
        write_disk_inode(entry_ino, target_disk);  // 还有别的硬链接,只写回新计数
    }
    write_disk_inode(parent_ino, dir_disk);
}
```

这里要专门指出一个和「写」对照的不对称,因为它最容易让人误解 028b 的能力。

`unlink` 释放数据块时,**老老实实地遍历了单间接块**——读出 `i_block[12]` 指向的间接块,把它指向的每一个数据块都 `free_block` 掉,最后连间接块本身也释放。也就是说,删除路径对单间接块的支持是完整的(12..267 全覆盖)。

可我们上一节刚说过,写路径(`Ext2FileOps::write`)到不了单间接块。这就形成一个尴尬的局面:**能删的块,你压根写不进去**。028b 的读支持单间接、删支持单间接,唯独写不支持——三条路径里写最弱。这不是 bug,是这一章的有意取舍(写大文件的需求还没有),但写正文时必须诚实交代,不能让读者以为 ext2 已经能写大文件然后又删掉。

另一个细节:`remove_dir_entry` 把目录项从父目录移除,但**不释放目录自己的数据块**(留到下一节讲)。`unlink` 释放的是「目标文件」的数据块,不是「父目录」的数据块。两者的粒度不一样。

还有一处真实的小妥协:`target_disk.i_dtime = 0;` 旁边跟着一行注释 `// TODO: use real timestamp when available`。ext2 规范里 `i_dtime` 是「删除时间」,但 Cinux 这会儿还没有实时时钟(RTC),所有时间戳(`i_atime`/`i_ctime`/`i_mtime` 也一样)都是 0。我们老实地留了个 TODO,而不是随便填个数假装有时间。

### 目录项增删:split rec_len 与留空洞

建文件、建目录都要往父目录里加一项,删除要移掉一项。这两个操作(`add_dir_entry`/`remove_dir_entry`)处理的,是 ext2 那种「变长、靠 `rec_len` 串联」的目录项布局(028 讲过)。这里的关键是**怎么在一个变长链表里塞进去、抠出来**。

先回忆目录项的结构:`inode(4)` + `rec_len(2)` + `name_len(1)` + `file_type(1)` + `name(name_len)`,整体长度 `rec_len`,而且 `rec_len` 一定是 4 的倍数。一个目录数据块就是一串这样的项首尾相接,靠每项的 `rec_len` 跳到下一项。

`add_dir_entry` 插一个新项,有两种情况。

**情况一:现有块里有空隙,塞进去。** ext2 的目录项通常不会排得很紧——最后一个项的 `rec_len` 往往比它实际需要的长(「实际需要」= `(8 + name_len)` 向上取整到 4),多出来的部分是「给未来留的空」。`add_dir_entry` 扫每个块,算每项的 `entry_min`(它真正需要的长度),看 `rec_len - entry_min` 这个空隙够不够放下新项:

```text
原块(最后一个项 rec_len 偏大,有空隙):
┌────────────┬──────────────────────────────┐
│ 旧项 A     │ 旧项 B  rec_len = 余下整块     │  ← B 实际只要 12 字节,却占了一大片
└────────────┴──────────────────────────────┘

插入新项 C(split):
┌────────────┬──────────┬───────────────────┐
│ 旧项 A     │ 旧项 B   │ 新项 C  rec_len=空隙 │  ← B 的 rec_len 缩到 12,C 接在后面
│            │ rec_len=12│                   │
└────────────┴──────────┴───────────────────┘
```

做法是 **split**:把当前项的 `rec_len` 缩成它实际需要的 `entry_min`,腾出的空间给新项,新项的 `rec_len` 吃掉剩下的余量。

**情况二:现有块塞不下,分配新块。** 所有块都没空隙,就 `alloc_block` 分一个新数据块,把新项写进去——而且这一项的 `rec_len` 直接设成**整个块**(它现在是这块里唯一的项,占满)。然后更新父目录 inode 的 `i_block[新索引]`、`i_size += block_size`。

这里有个和「写文件」一样的边界:**目录只走直接块**。`add_dir_entry` 找空隙时 `b < EXT2_DIRECT_BLOCKS`,分配新块时 `if (new_block_idx >= EXT2_DIRECT_BLOCKS) 报 "directory full"`。也就是说一个目录最多 12 个数据块(block_size=1024 时约 12KB 的目录项)。对教学系统够用,但别指望它装下几万个文件。

`remove_dir_entry` 抠掉一项,也分两种:

- **要删的是块里的第一项**(偏移 0):没法把它「合并回前一项」,因为前面没有项了。于是只把它的 `inode` 字段清成 0,标记这一格「废弃」,但 `rec_len` 保持不变——**留一个空洞**。
- **否则**:把它的 `rec_len` 加到前一项的 `rec_len` 上,相当于让前一项「吃掉」它,链表自然跳过它。

注意两种情况都**没有释放目录自己的数据块**。哪怕一个块里所有项都删空了,这块数据块还是挂在目录 inode 的 `i_block[]` 上,位图里也还标着「已用」。这是 028b 的一个简化:目录块不回收。后果是反复增删文件会让目录块越积越多(虽然内容是空洞),但对教学场景影响不大,而且避免了「回收目录块后还要调整 `i_size`、合并相邻空洞」的一堆麻烦。

### 接上用户态:sys_creat 的链路与 sys_rmdir 的空目录检查

底层都齐了,最后看 syscall 怎么把用户态的路径接到底层。`sys_creat` 是个范本,`sys_mkdir`/`sys_unlink` 结构几乎一样:

```cpp
int64_t sys_creat(uint64_t path_virt, ...) {
    // ① 校验用户态指针是合法的规范地址(canonical)
    if (path_virt == 0) return -1;
    /* bit47 / upper 检查,非规范地址直接拒 */

    // ② 经挂载表解析:得到具体的 FileSystem 和相对路径
    const char* rel_path = nullptr;
    cinux::fs::FileSystem* fs = cinux::fs::vfs_resolve(path, &rel_path);

    // ③ 把路径拆成「父目录路径」+「叶子名」
    char parent_buf[PATH_MAX];
    split_pathname(rel_path, parent_buf, &leaf_name, &name_len);
    //   "etc/foo/bar" -> parent="etc/foo", leaf="bar"
    //   "bar"         -> parent="" (根),    leaf="bar"

    // ④ 找到父目录 inode,调它的 create
    cinux::fs::Inode* parent = fs->lookup(parent_buf);
    cinux::fs::Inode* new_inode = parent->ops->create(parent, leaf_name, name_len);

    if (new_inode != nullptr) return 0;

    // ⑤ create 返回空:文件可能已存在 -> truncate 到 0(POSIX creat 语义)
    cinux::fs::Inode* existing = fs->lookup(rel_path);
    if (existing != nullptr) existing->size = 0;
    return 0;
}
```

两个点值得说。

第一,**指针校验**。用户传进来的是一个 `uint64_t` 地址,内核不能直接信。代码检查它是不是「规范地址」(x86_64 的 canonical address:第 47 位为 0 时高 16 位必须全 0,为 1 时必须全 1)。非规范地址访问会触发 #GP,所以这里提前挡掉。这是用户态和内核态打交道时的基本功——别拿用户给的指针当可信的。

第二,**`split_pathname` 这一步**。它找到路径里最后一个 `/`,把前面截成父目录、后面截成叶子名。注意 028b 里这个函数是**每个 syscall 文件内联一份**的(`sys_creat`/`sys_mkdir`/`sys_unlink`/`sys_rmdir` 各有一份几乎相同的 `split_pathname`)。重复,但能用。把它抽成公共的 `path.cpp`,是后面的事(别急,下一章再说)。

`sys_creat` 的第 ⑤ 步对应 POSIX `creat(2)` 的语义:`creat` 一个已存在的文件,不报错,而是把它截断成 0。所以 `create` 返回 `nullptr`(可能因为已存在)时,代码再 `lookup` 一次,把已有文件的 `size` 清零。

`sys_rmdir` 多一道**空目录检查**,而且这道检查的位置值得专门讲——它**在 syscall 层,不在 ext2 里**:

```cpp
int64_t sys_rmdir(uint64_t path_virt, ...) {
    // ...resolve、split、lookup parent...
    cinux::fs::Inode* target = fs->lookup(rel_path);
    if (target->type != InodeType::Directory) { /* 不是目录 */ return -1; }

    // 关键:用 readdir 取第 3 项(index 0=".", 1=".."),有就说明非空
    char check_name[16];
    int64_t rc = target->ops->readdir(target, 2, check_name, sizeof(check_name));
    if (rc > 0) { /* 目录非空 */ return -1; }

    return parent->ops->unlink(parent, leaf_name, name_len);
}
```

它利用了 ext2 `readdir` 的索引约定:`readdir(dir, 0)` 返回 `.`,`readdir(dir, 1)` 返回 `..`,从 `index = 2` 起才是真正的子项。于是 `readdir(target, 2)` 能读到东西(`rc > 0`),就说明这个目录除了 `.`/`..` 还有别的条目,非空,拒绝删除。这是个聪明但有点脆的办法——它强依赖 `readdir` 的 index 语义恰好是 0/1/2+。

这里有个文档和实现不一致的小坑要提醒:`sys_rmdir.cpp` 的文件头注释写着「the backend filesystem is responsible for verifying that the target is an empty directory」(空目录检查由后端文件系统负责)。但代码里检查明明是在 `sys_rmdir` 自己做的,底层的 `Ext2::unlink` 根本不区分文件和目录、也不查空——你拿它删一个非空目录,它会照删不误(把目录项和数据块全释放)。以代码为准:028b 的空目录检查在 syscall 层。直接绕过 `sys_rmdir` 调 `unlink` 的话,这道防线就不存在了。这是个值得记下来的「注释漂移」。

四个新系统调用的号码,我们直接复用了 Linux 的:`mkdir=83`、`rmdir=84`、`creat=85`、`unlink=87`。这样将来真要跑为 Linux 编译的简单用户程序,号码能对得上。用户态 `user/libc/syscall.h` 加了对应的 `sys_creat`/`sys_mkdir`/`sys_unlink`/`sys_rmdir` 封装,shell 则加了 `touch`(=creat)、`mkdir`、`rm`(=unlink)、`rmdir`,以及 `echo` 的 `>` 重定向——`echo hi > /hello.txt` 会先 `sys_creat` 建文件、`sys_open(O_WRONLY)` 打开、`sys_write` 写入、`sys_close` 关闭。

## 调试现场:sys_creat 第一次深入 ext2 时,触发了 General Protection Fault

这一章最有意思的一个坑,不在 ext2 逻辑里,而在它第一次被系统调用真正驱动起来的时候。过程值得完整走一遍,因为它是一个经典的「症状在深处、根因在入口」的对齐 bug。

**现象。** 把 028b 的写链路接好,头几次 `touch`、`mkdir` 都正常。直到在 QEMU 里执行 `touch /hello2.txt`,内核直接崩:

```text
==== EXCEPTION: #GP (vector 13) ====
  RIP   = 0xFFFFFFFF81005D97   CS  = 0x0010
  RFLAGS= 0x0000000000010002
  ERROR CODE = 0x0000000000000000
========================================
```

General Protection Fault(#GP,向量 13),错误码 0。错误码 0 通常意味着「不是因为段选择子问题,而是别的通用保护违例」——比如一条要求对齐的指令碰上了没对齐的地址。

**定位崩溃点。** 用 `nm` 把崩溃地址 `0x5D97` 反查回符号:

```text
ffffffff81005d20 T Ext2::create(uint32_t, char const*, uint32_t)
```

RIP 落在 `Ext2::create()` 内部。再反汇编那个地址附近:

```text
ffffffff81005d7d:  lea    0x80(%rsp),%rdx    ; 栈上 new_disk 的地址
ffffffff81005d8e:  mov    %rdx,%rax
ffffffff81005d97:  movaps %xmm0,(%rax)       ; ← #GP 就在这一条
```

`movaps` 是一条 SSE 指令,把一个 16 字节的 XMM 寄存器存到内存,**要求目标地址 16 字节对齐**。`%rax = %rsp + 0x80`,而当时 `%rsp` 末尾是 `...78`,`0x78 % 16 = 8`——没对齐。`movaps` 一执行就 #GP。

这条 `movaps` 是哪来的?正是 `Ext2::create` 里那段把新 `Ext2Inode` 清零的循环(就是上一节我们标出来的那句)。编译器发现「把一个结构体清零」可以用一条 `movaps` 把 16 字节的 `xmm0`(全 0)一次写进去,比一字节一字节快,于是这么生了码。代码没错,错的是**栈没对齐到 16 字节**。

**根因:syscall 入口没维护栈对齐。** 沿调用链往上回溯:`syscall_entry`(汇编)→ `syscall_dispatch`(C)→ ... → `Ext2::create`(C)。System V AMD64 ABI 有一条硬性要求:**在执行 `call` 指令的那一刻,RSP 必须是 16 的倍数**。`call` 自己会 push 8 字节返回地址,所以被调用者一进来时 RSP 是 `16k+8`,但它只要在每次 `call` 别人之前把栈重新对齐到 16 就行。这套约定保证了任何函数里,栈上局部变量的地址都能满足 `movaps` 这类指令的对齐要求——前提是**从入口开始,每一层都守规矩**。

Cinux 的 `syscall_entry` 是手写汇编,它没守。它 push 了 12 个寄存器构造 trap frame(96 字节),再 push 第 7 个 C 参数(8 字节),一共 13 次 push = 104 字节。`104 % 16 = 8`。于是在 `call syscall_dispatch` 之前,RSP 是 `16k+8` 而不是 `16k`——**差了 8 字节**。这个 8 字节的偏移一路传下去,到了 `Ext2::create` 里,栈上 `new_disk` 的地址就都偏了 8,本来该对齐的变得不对齐,`movaps` 一碰就炸。

**为什么之前的系统调用没事?** 这是最关键的问题,也是这类 bug 最坑的地方。023/027b 那些 syscall(`sys_read`/`sys_write`/`sys_open` 等)执行路径浅、调用链不深,而且没碰上要求 16 字节对齐的指令。对齐是错的,但「错而未爆」。`sys_creat` 是第一个**深入调到 ext2 复杂逻辑、且那里恰好有结构体清零**的 syscall,这才头一次把这条潜伏的对齐错误逼了出来。换句话说:bug 不在崩溃的 `Ext2::create` 里,而在最顶层的 `syscall_entry` 里;只是直到这里才暴露。

**修复。** 在 push 第 7 个参数之前,先 `subq $8, %rsp` 把栈补齐到 16:

```asm
    movq 72(%rsp), %rax                # 取出第 7 个参数
    subq $8, %rsp                      # ← 新增:把栈对齐到 16 字节
    pushq %rax                         # push 第 7 个参数

    movq 40(%rsp), %rdi                # 注意:所有偏移都 +8 变成 +16
    ...
    call syscall_dispatch
    addq $16, %rsp                     # 清理:参数 + 对齐填充,从 $8 改成 $16
```

多垫的那 8 字节,让 trap frame 总大小从 104 变成 112(`112 % 16 = 0`),`call` 前 RSP 终于对齐了。代价是 trap frame 里所有字段的读取偏移都得 `+8`(`movq 32(%rsp)` 变成 `movq 40(%rsp)`,以此类推),清理时的 `addq $8` 也得改成 `addq $16`。

**教训。** 三条,都值得记进经验:

1. **手写的入口(syscall、中断处理)必须维护 ABI 的栈对齐不变量。** SysV AMD64 假定 `call` 前 RSP ≡ 0 (mod 16)。任何手写汇编入口,只要它最后要 `call` 一段 C 代码,就得自己保证这一点,否则所有用 SSE/AVX 对齐指令的编译产物都可能随机崩。
2. **对齐 bug 有隐蔽性。** 崩在 `Ext2::create`,根因在 `syscall_entry`;调用浅的 syscall 不爆,只有调用深、且碰上对齐敏感指令时才暴露。看到 #GP + error_code=0,第一反应就该是「是不是哪里没对齐」。
3. **排查路径是固定的。** #GP err=0 → 反汇编 RIP,找 `movaps`/`movdqa` 这类对齐指令 → 算它目标地址对不对齐 → 沿调用链回溯到最顶层的汇编入口,数 push 次数、算 RSP 对齐状态。

## 验证

028b 的写测试分两层:host 单元测试(不碰真硬件,在内存镜像上跑 ext2 逻辑)和 QEMU kernel 测试(真跑、真写 AHCI 盘)。

**host 单元测试**,在 build 目录跑:

```bash
cmake --build build
ctest --test-dir build -R ext2 --output-on-failure
ctest --test-dir build -R ahci_write --output-on-failure
ctest --test-dir build -R shell_write --output-on-failure
```

覆盖面:`test_ext2_allocator`(块/inode 分配器——分配、释放、位图置位、计数同步)、`test_ext2_ops`(`creat`/`mkdir`/`write`/`unlink` 端到端)、`test_ext2_inode_ops`(经 `InodeOps::write`/`read`)、`test_syscall_ext2`(经系统调用号的端到端)、`test_ahci_write`(AHCI 写盘)、`test_shell_write`(shell 的 `touch`/`mkdir`/`rm`/`rmdir`/`echo >`)。

**QEMU kernel 测试**:

```bash
cmake --build build --target run-kernel-test
```

这里有个 028b 新增的、值得专门讲的工程细节:`run-kernel-test` 现在会**先强制重建 ext2 镜像**。看 `cmake/qemu.cmake`:

```cmake
add_custom_target(regenerate-ext2-image
    COMMAND ${CMAKE_COMMAND} -E remove -f ${EXT2_IMAGE}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/create_ext2_disk.sh ${EXT2_IMAGE}
    ...
)
add_custom_target(run-kernel-test
    ...
    DEPENDS test-image ${AHCI_TEST_IMAGE} regenerate-ext2-image
)
```

为什么?因为 028b 能写盘了。028 那会儿 ext2 是只读的,每次跑测试,盘上的内容都是 `create_ext2_disk.sh` 预先做好的固定样子,跑完不变。028b 不行——一次测试里 `touch` 出来的新文件、改过的位图和计数,会**真的写进 ext2.img**。如果不重建,下一次测试就在一个「被上次测试写脏过」的盘上跑,状态污染,结果不可复现。所以每跑一次 kernel 测试前,先 `rm` 掉旧镜像、用脚本重新生成一个干净的。这条依赖是 028b 写能力的直接副产品。

**预期现象。** 在 QEMU 的 shell 里(或测试里)做一串操作,验证写链路:

```text
$ touch /hello.txt            # sys_creat → Ext2::create
$ echo hi > /hello.txt        # creat + open + write
$ mkdir /somedir              # sys_mkdir → Ext2::mkdir
$ (读回 /hello.txt)           # 应得到 "hi"
$ rm /hello.txt               # sys_unlink → Ext2::unlink
$ rmdir /somedir              # sys_rmdir(空目录检查通过)→ unlink
```

写链路通的标志是「写进去的能读回来,内容一致」。至于「跨重启持久」——因为 `write_block` 经 `ahci.write` 把数据真的写进了 raw 盘镜像,持久性是这条链路的自然结果;只是 CI 为了测试隔离,默认每次 `regenerate` 一个干净盘,所以「持久」要在不重建镜像的两次运行之间才能直接观察到。

**故意触发那个 GP fault 复现一下**(在修复前的版本上)也能验证调试现场的结论:`touch /hello2.txt` 会让未打补丁的内核在 `Ext2::create` 里 #GP,打上 `syscall.S` 的 `subq $8, %rsp` 后消失。

## 下一站

028b 让 ext2 能建、能写、能删,文件系统的「写」这一半补齐了。但你会发现 shell 现在有个别扭的地方:所有命令都得用从根开始的绝对路径——`/hello.txt`、`/somedir`。没有「当前工作目录」的概念,也就没有 `cd`、`pwd`;想看一个文件的大小、类型这些 inode 信息,也没有 `stat`。这些是文件系统作为一个「给人用的」系统还缺的拼图。下一章(028c)会补上工作目录和 `stat`,顺带把这一章里那个每个 syscall 各写一份的 `split_pathname` 抽成公共的路径解析模块。怎么抽、cwd 挂在进程的哪里,那是 028c 的事——我们这一章的 ext2,已经能老老实实地往磁盘上写东西了。

---

**参考**

- ext2 磁盘布局规范(The Second Extended Filesystem):块位图/inode 位图的位语义、`i_block[15]` 的直接/单间接/双间接/三间接划分、目录项 `rec_len`+`name_len` 布局、`i_links_count` 与 `bg_used_dirs_count` 的维护规则。这些是本章分配器、目录项增删、`mkdir` 链接计数的依据(028 已引,本章沿用)。
- System V AMD64 ABI:在执行 `call` 指令时 RSP 必须是 16 字节对齐——这是「调试现场」里 GP fault 根因的依据。参考 x86-64 psABI:<https://gitlab.com/x86-psABIs/x86-64-ABI>。
- ATA/ATAPI Command Set(ACS):`WRITE DMA EXT`(命令码 0x35,48 位 LBA),AHCI `write` 走的就是它(见 `ahci.hpp`)。
- Linux man-pages:`creat(2)`(已存在则截断为 0)、`mkdir(2)`/`rmdir(2)`(目录须为空)、`unlink(2)`(链接数归零则释放)的语义,以及 syscall 号 83/84/85/87 的复用:<https://man7.org/linux/man-pages/>。
