---
title: 05 · 写文件:直接块、单间接、截断的循环
---

# 写文件:直接块、单间接、截断的循环

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

`get_or_alloc_block` 能处理直接块(0..11)和单间接块(12..267,一个间接块装 `1024/4 = 256` 个指针)。但 `Ext2FileOps::write` 的循环在 `file_block > 12` 时就 break 了,根本到不了单间接块的第 2 个槽(index 13)。换句话说,**单间接块那 256 个槽,写路径只用了第 1 个**。这是 006 一个真实的、有意识的小局限:读(005)支持完整的单间接,写(006)却卡在第 13 块。够 `echo` 写点配置、够测试验证写链路,但写不了大文件。我们没有把它伪装成「支持大文件」——`get_or_alloc_block` 留着单间接分支,是因为删除路径要用它(下一节),而不是因为写路径真的用到了。
