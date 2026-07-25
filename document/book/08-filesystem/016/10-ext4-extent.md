---
title: 10 · 主线九:ext4 extent 读路径
---

# 主线九:ext4 extent 读路径

## 主线九:ext4 extent 读路径——挂在 `resolve_disk_block_` 前面的另一条解析器

搬进 `libs/ext2/` 的除了 ext2 自己,还有一段 ext4 的读路径——extent tree 解析器(`libs/ext2/ext2_extent.cpp`)。它不是这章 race 绳的一部分,可它**接在 `resolve_disk_block_` 的入口**,跟前面讲的「读 indirect 指针」是同一条解析链上的岔路,得讲清它怎么岔、为什么这么接。

### extent 是什么:把 60 字节的 `i_block` 当成树根

经典 ext2 的 `i_block[0..14]` 是**块指针数组**:12 个直接指针 + 1 个 indirect + 1 个 double-indirect + 1 个 triple(本驱动不支持 triple)。ext4 给这个区域换了一种解释——如果 inode 的 `i_flags` 里设了 `EXT4_EXTENTS_FL`,这 60 字节就不再是块指针,而是一棵 **extent tree 的根**:一段 12 字节的头,后面跟一组 extent(叶)或一组 index(指向下一层)。

```cpp
/// Superblock incompatible-feature bit: filesystem uses per-inode extent trees
static constexpr uint32_t EXT4_FEATURE_INCOMPAT_EXTENTS = 0x40;

/// Inode flag: i_block[0..14] holds an extent tree (not classic block pointers)
static constexpr uint32_t EXT4_EXTENTS_FL = 0x80000;

/// Magic value stored in Ext4ExtentHeader::eh_magic
static constexpr uint16_t EXT4_EXTENT_MAGIC = 0xF30A;
```

([ext2_types.hpp:354-361](../../../libs/ext2/ext2_types.hpp#L354))。一个**叶 extent** 就是一条「连续的逻辑块 → 连续的物理块」映射:

```cpp
/**
 * @brief ext4 leaf extent (depth 0): a contiguous logical→physical block run
 *
 * A leaf node is eh_max Ext4Extent entries (12 bytes each) immediately after
 * the Ext4ExtentHeader.  Covers logical blocks [ee_block, ee_block + len).
 */
struct [[gnu::packed]] Ext4Extent {
    uint32_t ee_block;     ///< First logical block this extent covers
    uint16_t ee_len;       ///< Block count (>32768 ⇒ uninitialized, len = ee_len-32768)
    uint16_t ee_start_hi;  ///< High 16 bits of physical start block
    uint32_t ee_start_lo;  ///< Low 32 bits of physical start block
};
```

([ext2_types.hpp:384-397](../../../libs/ext2/ext2_types.hpp#L384))。意思是「从逻辑块 `ee_block` 起、连续 `ee_len` 个块,对应的物理块从 `(ee_start_hi << 32) | ee_start_lo` 开始」。一条 extent 就能覆盖一大段连续数据(比如一个 1 MiB 的文件,1 KB 块就是 1024 个块,一条 extent 搞定),比 indirect 指针数组(每个块号都得占 4 字节、还得读一个 indirect 块)省盘、省 I/O。

### depth-0 leaf:直接给块号,不读盘

`extent_lookup_block` 干的活就是:拿着 file 的逻辑块号,在这棵 depth-0 的 tree 里找覆盖它的那条 extent,算出物理块号。整段**不读盘**——extent tree 的根就在 inode 的 `i_block` 里,已经在内存了:

```cpp
ExtentLookupResult extent_lookup_block(const Ext2Inode& disk, uint32_t file_block,
                                       uint32_t& out_block) {
    // The extent tree root occupies the full 60-byte i_block[0..14] region.
    auto* tree = reinterpret_cast<const uint8_t*>(disk.i_block);
    auto* hdr  = reinterpret_cast<const Ext4ExtentHeader*>(tree);

    if (hdr->eh_magic != EXT4_EXTENT_MAGIC) {
        // Flagged extent-based but the header is absent/corrupt: do not guess.
        return ExtentLookupResult::Unsupported;
    }
    if (hdr->eh_depth != 0) {
        // Index nodes (depth > 0) need a follow-up reader; bail honestly.
        return ExtentLookupResult::Unsupported;
    }

    auto*    extents = reinterpret_cast<const Ext4Extent*>(tree + sizeof(Ext4ExtentHeader));
    uint16_t count   = hdr->eh_entries;

    for (uint16_t i = 0; i < count; ++i) {
        const Ext4Extent& e      = extents[i];
        uint32_t          log    = e.ee_block;
        uint16_t          raw    = e.ee_len;
        bool              uninit = raw > EXT4_EXTENT_INIT_LEN_MAX;
        // Uninitialized extents encode real length as ee_len - 32768.
        uint32_t len = uninit ? static_cast<uint32_t>(raw - EXT4_EXTENT_INIT_LEN_MAX) : raw;

        if (file_block >= log && file_block < log + len) {
            if (uninit) {
                // Preallocated-but-unwritten region: reads return zeros.
                return ExtentLookupResult::Hole;
            }
            uint64_t phys_start = (static_cast<uint64_t>(e.ee_start_hi) << 32) | e.ee_start_lo;
            out_block           = static_cast<uint32_t>(phys_start + (file_block - log));
            return ExtentLookupResult::Mapped;
        }
    }

    // No extent covers this logical block: a hole (sparse file) → zero-fill.
    return ExtentLookupResult::Hole;
}
```

([ext2_extent.cpp:18-57](../../../libs/ext2/ext2_extent.cpp#L18))。三个 outcome 得分清(枚举在 [ext2_extent.hpp:31-35](../../../libs/ext2/ext2_extent.hpp#L31)):

- `Mapped`——找到了覆盖的 extent,`out_block` 里是物理块号,调用方去读;
- `Hole`——逻辑块没被任何 extent 覆盖(稀疏文件的洞),或者命中了一条 **uninitialized extent**(`ee_len > 32768`,意思是这块盘空间预分配了但没写,read 该返回零);
- `Unsupported`——magic 不对(标了 extent flag 但 header 损坏),或者 `eh_depth > 0`(树还有 index 层,本驱动不读)。这俩都**诚实地 bail**,不猜——`return 0` 让上层停读,而不是瞎给个块号去 I/O。

`ee_len > EXT4_EXTENT_INIT_LEN_MAX`(32768)那条是 ext4 uninitialized extent 的编码:真实长度 = `ee_len - 32768`,读这块逻辑块该返回零。代码把它判成 `Hole`(zero-fill),逻辑等价——洞和未写区域对 read 的语义都是「全零」。

### 接进 `resolve_disk_block_`:extent 在前、indirect 在后

extent 解析器是**挂在 `resolve_disk_block_` 的入口**的,不是平行分支。它得在前:

```cpp
uint32_t Ext2FileOps::resolve_disk_block_(const Ext2Inode& disk, uint64_t file_block,
                                          uint64_t block_ptrs_per_block, uint8_t* scratch) {
    const uint32_t blocks_count = ext2_.blocks_count();
    if (inode_has_extent_tree(disk)) {
        uint32_t           extent_block = 0;
        ExtentLookupResult r =
            extent_lookup_block(disk, static_cast<uint32_t>(file_block), extent_block);
        uint32_t blk = (r == ExtentLookupResult::Mapped) ? extent_block : 0;
        ext2_trace_wild_blk(blk, file_block, disk, blocks_count);
        return blk;
    }
    if (file_block < EXT2_DIRECT_BLOCKS) {
        uint32_t blk = disk.i_block[file_block];
        ...
```

([ext2_common.cpp:196-211](../../../libs/ext2/ext2_common.cpp#L196))。`inode_has_extent_tree(disk)` 就是查 `i_flags & EXT4_EXTENTS_FL`([ext2_extent.hpp:26-28](../../../libs/ext2/ext2_extent.hpp#L26))——一个 inline 谓词,不读盘。如果 inode 是 extent-mapped 的,**整段 indirect 路径都不走**(`i_block` 已经被重解释成 extent tree 了,再当块指针读就是垃圾),直接调 `extent_lookup_block` 拿块号;否则才回退到经典的 direct/indirect/double-indirect 解析。

注意 extent 这条岔路**不碰 `scratch`**——depth-0 的 extent tree 根在 inode 里、不读盘,所以不需要中间 buffer。这是 extent 跟 indirect 在 SMP 上的一个本质区别:indirect 要 `read_block` 一个 indirect 块、所以必须有自己的 `KmBuf`(主线四讲过);extent depth-0 leaf 是纯算术,无 I/O,无 buffer,自然也就没有 `block_buf_` race 那一层。当然,如果 extent 树是 `depth > 0`(有 index 节点),那就要读 index 块,又会引入 buffer 问题——但这一层本驱动 `Unsupported`,不在这次治理范围里。

### 怎么验:QEMU in-kernel 测一个真 ext4 卷

extent 这条路径没法靠 host 测验(预构镜像是 ext2 的),它在 QEMU 里跑一张专门的 ext4 镜像——`kernel/test/test_ext4_extents.cpp` 挂 AHCI port 2 上那张 ext4 盘(由 `scripts/create_ext4_disk.sh` 构造),验三件事:

1. **挂载能识别 ext4 extents 特性**:卷的 superblock 设了 `EXT4_FEATURE_INCOMPAT_EXTENTS`,`has_ext4_extents_feature()` 返回真([test_ext4_extents.cpp:96-105](../../../kernel/test/test_ext4_extents.cpp#L96));
2. **大文件(1 MiB)走 extent 且读回字节精确**:`/big.bin` 是 1 MiB、一条 depth-0 leaf extent(1024 块 @ 1 KB),整段读回验 `byte[i] == i & 0xFF`([test_ext4_extents.cpp:131-172](../../../kernel/test/test_ext4_extents.cpp#L131)),还专门测一段跨块边界的读([test_ext4_extents.cpp:174-191](../../../kernel/test/test_ext4_extents.cpp#L174))——验 extent 解析的块内偏移算术;
3. **小文件(单块 extent)也能读**:`/small.txt` 单块 extent,读回 `"ext4 extents small file\n"`([test_ext4_extents.cpp:201-217](../../../kernel/test/test_ext4_extents.cpp#L201))。

这条测的关键是它**先验 inode 真的是 extent-mapped**(`cached->disk_inode.i_flags & EXT4_EXTENTS_FL`),再读——不然读对了也可能是走了 indirect 路径的巧合:

```cpp
// The inode must actually be extent-mapped -- otherwise the read below would
// fall through to the (wrong) indirect-block path.
auto* cached = static_cast<const Ext2CachedInode*>(ino->fs_private);
TEST_ASSERT_TRUE((cached->disk_inode.i_flags & EXT4_EXTENTS_FL) != 0);
```

([test_ext4_extents.cpp:124-126](../../../kernel/test/test_ext4_extents.cpp#L124))。这层前置断言把「extent 路径真的被走到了」钉死,避免误判。

> **目录扫描走 `inode_read_block`,不是 `resolve_disk_block_`**。extent 解析还有个共用入口 `inode_read_block`([ext2_extent.cpp:59-71](../../../libs/ext2/ext2_extent.cpp#L59)),它先判 extent,否则回退到 direct(`i_block[0..11]`)。`lookup_in_dir` / `readdir` 这种目录扫描用这个——目录通常很小,只在 direct 区,`inode_read_block` 一行就解析了。而常规文件读走 `resolve_disk_block_` 那条带 indirect/extent 双岔路的完整解析。两个入口共用 `extent_lookup_block`,分工看场景。
