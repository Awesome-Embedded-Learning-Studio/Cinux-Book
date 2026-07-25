---
title: 04 · 两个分配器:扫位图找空闲
---

# 两个分配器:扫位图找空闲

## 两个分配器:扫位图找空闲

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
