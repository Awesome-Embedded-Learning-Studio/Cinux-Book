---
title: 07 · 删除与目录项增删:unlink 释放、split rec_len
---

# 删除与目录项增删:unlink 释放、split rec_len

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

这里要专门指出一个和「写」对照的不对称,因为它最容易让人误解 006 的能力。

`unlink` 释放数据块时,**老老实实地遍历了单间接块**——读出 `i_block[12]` 指向的间接块,把它指向的每一个数据块都 `free_block` 掉,最后连间接块本身也释放。也就是说,删除路径对单间接块的支持是完整的(12..267 全覆盖)。

可我们上一节刚说过,写路径(`Ext2FileOps::write`)到不了单间接块。这就形成一个尴尬的局面:**能删的块,你压根写不进去**。006 的读支持单间接、删支持单间接,唯独写不支持——三条路径里写最弱。这不是 bug,是这一章的有意取舍(写大文件的需求还没有),但写正文时必须诚实交代,不能让读者以为 ext2 已经能写大文件然后又删掉。

另一个细节:`remove_dir_entry` 把目录项从父目录移除,但**不释放目录自己的数据块**(留到下一节讲)。`unlink` 释放的是「目标文件」的数据块,不是「父目录」的数据块。两者的粒度不一样。

还有一处真实的小妥协:`target_disk.i_dtime = 0;` 旁边跟着一行注释 `// TODO: use real timestamp when available`。ext2 规范里 `i_dtime` 是「删除时间」,但 Cinux 这会儿还没有实时时钟(RTC),所有时间戳(`i_atime`/`i_ctime`/`i_mtime` 也一样)都是 0。我们老实地留了个 TODO,而不是随便填个数假装有时间。

### 目录项增删:split rec_len 与留空洞

建文件、建目录都要往父目录里加一项,删除要移掉一项。这两个操作(`add_dir_entry`/`remove_dir_entry`)处理的,是 ext2 那种「变长、靠 `rec_len` 串联」的目录项布局(005 讲过)。这里的关键是**怎么在一个变长链表里塞进去、抠出来**。

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

注意两种情况都**没有释放目录自己的数据块**。哪怕一个块里所有项都删空了,这块数据块还是挂在目录 inode 的 `i_block[]` 上,位图里也还标着「已用」。这是 006 的一个简化:目录块不回收。后果是反复增删文件会让目录块越积越多(虽然内容是空洞),但对教学场景影响不大,而且避免了「回收目录块后还要调整 `i_size`、合并相邻空洞」的一堆麻烦。
