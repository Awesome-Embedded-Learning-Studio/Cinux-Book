---
title: 08 · 主线七:内核 parity——truncate 虚槽 + invalidate_range
---

# 主线七:内核 parity——truncate 虚槽 + invalidate_range

## 主线七:内核 parity 补两块——truncate 虚槽 + `invalidate_range`

ext2 搬独立库、治成 SMP-safe 的过程中,带出了两个 ext2 依赖、但内核原本没有的接口——得把它们补到 parity(对齐),新 ext2 才编得过、跑得对。这是「换零件连带换接口」的真实工程连带。

**第一块:`InodeOps::truncate` 虚槽**。

```cpp
/// Set the file length to @p new_size (sys_open O_TRUNC / ftruncate).
/// Shrink-only for the O_TRUNC case (new_size 0): the backend updates the
/// on-disk + VFS size; freeing the now-orphaned data blocks is a follow-up
/// (a hobby-os leak, not a correctness issue -- reads stop at i_size).  The
/// default returns NotImplemented; only ext2 overrides for now.
virtual cinux::lib::ErrorOr<void> truncate(Inode* inode, uint64_t new_size);
```

([inode.hpp:92-97](../../../kernel/fs/inode.hpp#L92))。默认是 `NotImplemented`([inode.cpp:30](../../../kernel/fs/inode.cpp#L30)),ext2 override 它来实现 `O_TRUNC` / `ftruncate` 的截断语义:

```cpp
cinux::lib::ErrorOr<void> Ext2FileOps::truncate(Inode* inode, uint64_t new_size) {
    ...
    // Shrink-only (O_TRUNC -> 0, or ftruncate down).  Growing would need
    // zero-fill + block alloc; not required for O_TRUNC.  We do NOT free the
    // now-orphaned data blocks (hobby-os leak, not a correctness issue: reads
    // stop at i_size, and a later write reuses the same blocks via
    // get_or_alloc_block).  Cache invalidation is unnecessary too: read_bytes
    // gates on inode->size (so post-truncate reads return 0 past new_size), and
    // the next write's invalidate_range refreshes pages with the new bytes.
    if (new_size < disk.i_size) {
        disk.i_size = static_cast<uint32_t>(new_size);
        if (!ext2_.write_disk_inode(static_cast<uint32_t>(inode->ino), disk)) {
            return cinux::lib::Error::IOError;
        }
    }
    inode->size = disk.i_size;
    return {};
}
```

([ext2_common.cpp:343-365](../../../libs/ext2/ext2_common.cpp#L343))

注意它是 **shrink-only**——只处理 `new_size` 比原 `i_size` 小的情况(典型 `sys_open` 带 `O_TRUNC` 的 `new_size=0`)。截断掉的那部分**孤儿数据块不释放**,是已知的 hobby-os 式 leak:read 不超过 `i_size`(读不到那些块),后续 write 走 `get_or_alloc_block` 会复用同一批块,所以不影响正确性,只浪费磁盘。诚实写进边界,留 follow-up。

**第二块:`PageCache::invalidate_range`**。

ext2 的 write **旁路 page cache 直写盘**(write 不经过 cache)。但如果某个被覆盖的页正好在 cache 里(之前被 read 进来过),cache 里就是 stale 字节,后续 read 会读到旧数据。`invalidate_range` 在 write 直写盘后刷新覆盖区间的缓存页,保证一致性:

```cpp
void PageCache::invalidate_range(cinux::fs::Inode* inode, uint64_t file_off, uint64_t count) {
    if (inode == nullptr || inode->ops == nullptr || count == 0) {
        return;
    }
    const uint64_t ps    = cinux::arch::PAGE_SIZE;
    const uint64_t mask  = ps - 1;
    const uint64_t start = file_off & ~mask;
    const uint64_t last  = (file_off + count - 1) & ~mask;
    for (uint64_t off = start;; off += ps) {
        CachedPage* p = nullptr;
        {
            // Lookup under the lock; the disk re-read below runs outside it
            // (no I/O under the cache lock -- F2-M4 GOTCHA).
            auto g = lock_.irq_guard();
            p      = lookup_locked(inode, off);
        }
        if (p != nullptr) {
            // Refresh the page in place: same physical page, so PTEs that
            // currently map it stay valid and just observe the new bytes.
            // memset first so a short read (EOF tail) stays zero-padded,
            // mirroring get_page()'s initial fill.
            void* v = reinterpret_cast<void*>(p->virt);
            memset(v, 0, ps);
            static_cast<void>(inode->ops->read(inode, off, v, ps));
        }
        if (off == last) { break; }
    }
}
```

([page_cache.hpp:129](../../../kernel/mm/page_cache.hpp#L129) 声明,[page_cache.cpp:162-191](../../../kernel/mm/page_cache.cpp#L162) 实现)

这两个为什么是 ext2 的依赖而非独立 feature?因为新 ext2 的行为(`O_TRUNC` 走 `truncate`、write 直写盘)需要它们做前提,不补 ext2 就跑不对。补到 parity 是搬家的连带账,不是另外的功能扩展。
