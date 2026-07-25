---
title: 06 · 建文件与建目录:create / mkdir 的资源管理与回滚
---

# 建文件与建目录:create / mkdir 的资源管理与回滚

## 建文件与建目录:create / mkdir 的资源管理与回滚

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
