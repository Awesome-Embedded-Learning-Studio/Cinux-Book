---
title: 03 · 写回:read-modify-write 与唯一的 DMA 缓冲
---

# 写回:read-modify-write 与唯一的 DMA 缓冲

## 写回的统一姿势:read-modify-write 与那块唯一的 DMA 缓冲

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

这条规律贯穿 006 的所有元数据写:

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

读—改—写有个隐含的硬约束,值得单独点出来:**整个 ext2 共用那一块 DMA 缓冲,而一次 read-modify-write 要用到它两次(读一次、写一次)**。这意味着在这些操作期间,中间不能插入任何别的块 I/O,否则缓冲内容会被覆盖,写回去的就是错的。006 没有并发(单核、无抢占、syscall 同步执行),所以暂时相安无事;但这根弦要记着——将来要是加了中断驱动的异步 I/O 或预读,这块单缓冲就是第一个要拆掉的东西。
