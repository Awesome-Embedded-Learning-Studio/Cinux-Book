/**
 * @file kernel/fs/ext2_common.cpp
 * @brief Ext2FileOps and Ext2DirOps implementations
 *
 * VFS InodeOps wrappers that delegate to the Ext2 driver for file
 * read/write/stat and directory readdir/create/mkdir/unlink/stat.
 */

#include <stddef.h>
#include <stdint.h>

#include "ext2.hpp"
#include "ext2_extent.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/lib/string.hpp"
#ifndef CINUX_HOST_TEST
#    include "kernel/drivers/hpet/hpet.hpp"  // g_hpet: read I/O timing (B2.5 stats)
#    include "kernel/arch/x86_64/backtrace.hpp"  // F-DYN-COV: wild-block trace
#    include "kernel/mm/page_cache.hpp"
#endif

namespace cinux::fs {

// B2.5: cumulative ext2 read I/O accounting for the periodic stats dump. The
// gcc-compile curve (after B2's lazy load) still shows a ~5 s residual stall;
// this attributes it to demand-PF I/O vs something else. handle_pf is IF=0 but
// -smp 2 can fault concurrently, so the counters are atomic. Declared in
// ext2.hpp; consumed by dump_memory_stats.
namespace {
uint64_t g_ext2_read_count = 0;
uint64_t g_ext2_read_bytes = 0;
uint64_t g_ext2_read_ns    = 0;

/// RAII: time Ext2FileOps::read() from entry to exit (counts every return path).
class Ext2ReadTimer {
  public:
#ifndef CINUX_HOST_TEST
    Ext2ReadTimer() : t0_(cinux::drivers::g_hpet.monotonic_ns()) {}
    ~Ext2ReadTimer() {
        __atomic_fetch_add(&g_ext2_read_ns, cinux::drivers::g_hpet.monotonic_ns() - t0_,
                           __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_ext2_read_count, 1, __ATOMIC_RELAXED);
    }
#else
    // Host build: no HPET device. Keep the read counter (host tests may inspect it)
    // but skip ns timing -- the host PAL does not provide g_hpet.
    Ext2ReadTimer() : t0_(0) {}
    ~Ext2ReadTimer() { __atomic_fetch_add(&g_ext2_read_count, 1, __ATOMIC_RELAXED); }
#endif

  private:
    uint64_t t0_;
};
}  // namespace

uint64_t ext2_read_count() { return __atomic_load_n(&g_ext2_read_count, __ATOMIC_RELAXED); }
uint64_t ext2_read_bytes() { return __atomic_load_n(&g_ext2_read_bytes, __ATOMIC_RELAXED); }
uint64_t ext2_read_ns()    { return __atomic_load_n(&g_ext2_read_ns,    __ATOMIC_RELAXED); }

// ============================================================
// Ext2FileOps
// ============================================================

Ext2FileOps::Ext2FileOps(Ext2& ext2) : ext2_(ext2) {}

bool Ext2FileOps::is_page_cacheable() const {
    return true;
}

cinux::lib::ErrorOr<int64_t> Ext2FileOps::read(const Inode* inode, uint64_t offset, void* buf,
                                               uint64_t count) {
    Ext2ReadTimer timer;  // B2.5: account this read's wall time to g_ext2_read_ns
    if (inode == nullptr || inode->fs_private == nullptr || buf == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }

    auto*            cached = ext2_cached_inode(inode);
    const Ext2Inode& disk   = cached->disk_inode;

    if (offset >= disk.i_size) {
        return 0;
    }

    uint64_t available = disk.i_size - offset;
    uint64_t to_read   = (count < available) ? count : available;

    if (to_read == 0) {
        return 0;
    }

    uint32_t bs                   = ext2_.block_size();
    uint64_t block_ptrs_per_block = bs / sizeof(uint32_t);

    // SMP-safe per-call scratch (block_buf_ is shared/non-thread-safe; two CPUs
    // demand-page-reading would clobber each other -> wild block numbers). Heap
    // not stack: #PF runs on IST2 which is only 4 KB (IRQ_STACK_PAGES=1).
    KmBuf scratch(4096);
    if (!scratch) {
        return cinux::lib::Error::IOError;  // slab OOM
    }

    auto*    dst        = static_cast<uint8_t*>(buf);
    uint64_t total_read = 0;

    while (total_read < to_read) {
        uint64_t file_block   = (offset + total_read) / bs;
        uint64_t block_offset = (offset + total_read) % bs;
        uint64_t chunk        = bs - block_offset;
        if (chunk > to_read - total_read) {
            chunk = to_read - total_read;
        }

        uint32_t disk_block = resolve_disk_block_(disk, file_block, block_ptrs_per_block, scratch.data());
        if (disk_block == 0) {  // hole / unmapped / extent-unsupported -> zero fill
            for (uint64_t i = 0; i < chunk; ++i) {
                dst[total_read + i] = 0;
            }
            total_read += chunk;
            continue;
        }

        // B3a agg OFF (follow-up): agg>1 read_disk_range SIGILLs ld-linux at 0x377e (a
        // legal `mov %rdx,0x8(%rax)`). Exhaustive trace: agg-on .text dst is byte-identical
        // to host (verified at the SIGILL point 0x77e == 48 89 50 08 and all 4 block heads
        // of ld-linux page 0x3000); read_disk_range dst == read_block block_buf_ for the
        // same disk_block; ld-linux .dynamic/.data agg=1 (fragmented, per-block == agg-off).
        // Yet agg-on executes wrong (rax from .bss _rtld_global+0xb50 is 0 -- ld-linux's
        // relocate never set it). Logic is airtight, data is airtight, but execution
        // diverges. Suspect an agg>1 side effect beyond dst (cache-page/refcount write?
        // AHCI dma_buf_ shared state? ldso bring-up timing?). Keep agg=1 (== pre-B3) so the
        // kernel works; re-enable after a deeper trace.
        uint64_t agg = 1;
        if (block_offset == 0) {  // B3a: agg re-enabled (audit+repair below)
            while (agg < 4 && total_read + agg * bs <= to_read) {
                uint32_t next = resolve_disk_block_(disk, file_block + agg, block_ptrs_per_block, scratch.data());
                if (next != disk_block + agg) break;
                agg++;
            }
        }

        if (agg > 1) {
            // Coalesce agg contiguous blocks via one DMA into block_buf_
            // (scratch), then copy only the bytes the caller asked for. The
            // tail of the last block can extend past `to_read` when count is
            // not a block multiple (ELF PT_LOAD straddle pages, short reads) --
            // writing those extra bytes to dst overruns the caller's buffer and
            // clobbers adjacent data. That overrun was the B3a SIGILL: ldso's
            // PT_LOAD RW straddle page (0x1003c000) is eager zero-filled, then
            // read() fills the file tail (count 0x84c); the old agg>1 path DMA'd
            // agg*bs = 2 KiB straight into dst, overwriting the .bss half
            // (offset 0x84c..0xfff) with file bytes. _rtld_global+0xb50 then
            // read a non-zero file byte as rax -> non-canonical -> #GP. Land the
            // DMA in block_buf_ first, then a bounded memcpy.
            if (!ext2_.read_disk_range(disk_block, agg, scratch.get()).ok()) break;
            uint64_t take = agg * bs;
            if (take > to_read - total_read) {
                take = to_read - total_read;
            }
            memcpy(dst + total_read, scratch.data(), take);
            total_read += take;
        } else {
            if (!ext2_.read_block(disk_block, scratch.get())) break;
            const auto* src = scratch.data() + block_offset;
            memcpy(dst + total_read, src, chunk);
            total_read += chunk;
        }
    }

    __atomic_fetch_add(&g_ext2_read_bytes, total_read, __ATOMIC_RELAXED);
    return static_cast<int64_t>(total_read);
}

// B3a: resolve file_block → on-disk block (0 = hole / unmapped / extent-unsupported /
// I/O error). Extracted from read() so the coalescing loop can probe N contiguous
// blocks. Clobbers block_buf_ (indirect-table reads); data I/O uses read_disk_range.
// Note: extent depth>0 (Unsupported) collapses to 0 here (zero-fill) -- read() used
// to break on it; ext4 depth>0 stays a follow-up and cc1's depth-0 extents are fine.
// F-DYN-COV trace (temporary, wild-block root-cause hunt): a blk >= s_blocks_count
// means disk.i_block[] or an indirect-block pointer is garbage.  Print which
// pointer + the disk_inode address + caller stack so the race source shows.
static void ext2_trace_wild_blk(uint32_t blk, uint64_t file_block, const Ext2Inode& disk,
                                uint32_t blocks_count) {
    if (blk == 0 || blk < blocks_count) {
        return;
    }
    cinux::lib::kprintf(
        "[EXT2-TRACE] resolve wild blk=%u file_block=%lu i_block[12]=%u [13]=%u [14]=%u disk=%p\n",
        blk, static_cast<unsigned long>(file_block), disk.i_block[EXT2_INDIRECT_BLOCK],
        disk.i_block[EXT2_DOUBLE_INDIRECT_BLOCK], disk.i_block[14],
        static_cast<const void*>(&disk));
#ifndef CINUX_HOST_TEST
    cinux::arch::backtrace();
#endif
}

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
        ext2_trace_wild_blk(blk, file_block, disk, blocks_count);
        return blk;
    }
    if (file_block < EXT2_DIRECT_BLOCKS + block_ptrs_per_block) {
        const uint32_t indirect_block = disk.i_block[EXT2_INDIRECT_BLOCK];
        if (indirect_block == 0) return 0;
        if (!ext2_.read_block(indirect_block, scratch)) return 0;
        const auto* indirect = reinterpret_cast<const uint32_t*>(scratch);
        uint32_t    blk      = indirect[file_block - EXT2_DIRECT_BLOCKS];
        ext2_trace_wild_blk(blk, file_block, disk, blocks_count);
        return blk;
    }
    if (file_block < EXT2_DIRECT_BLOCKS + block_ptrs_per_block +
                         block_ptrs_per_block * block_ptrs_per_block) {
        const uint32_t double_block = disk.i_block[EXT2_DOUBLE_INDIRECT_BLOCK];
        if (double_block == 0) return 0;
        if (!ext2_.read_block(double_block, scratch)) return 0;
        const uint32_t di_offset =
            static_cast<uint32_t>(file_block - EXT2_DIRECT_BLOCKS - block_ptrs_per_block);
        const uint32_t idx1           = di_offset / block_ptrs_per_block;
        const uint32_t idx2           = di_offset % block_ptrs_per_block;
        const auto*    double_ptrs    = reinterpret_cast<const uint32_t*>(scratch);
        const uint32_t indirect_block = double_ptrs[idx1];
        if (indirect_block == 0) return 0;
        if (!ext2_.read_block(indirect_block, scratch)) return 0;
        const auto* child_ptrs = reinterpret_cast<const uint32_t*>(scratch);
        uint32_t    blk        = child_ptrs[idx2];
        ext2_trace_wild_blk(blk, file_block, disk, blocks_count);
        return blk;
    }
    return 0;  // triple-indirect unsupported
}

cinux::lib::ErrorOr<int64_t> Ext2FileOps::write(Inode* inode, uint64_t offset, const void* buf,
                                                uint64_t count) {
    if (inode == nullptr || inode->fs_private == nullptr || buf == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }

    if (count == 0) {
        return 0;
    }

    auto*      cached = ext2_cached_inode(inode);
    Ext2Inode& disk   = cached->disk_inode;

    uint32_t bs = ext2_.block_size();

    // Highest logical block we can resolve: direct + single-indirect +
    // double-indirect.  Beyond this (triple-indirect, i_block[14]) the driver
    // does not allocate, so stop instead of silently truncating mid-chunk.
    uint64_t ptrs_per_block = bs / sizeof(uint32_t);
    uint64_t max_file_block = EXT2_DIRECT_BLOCKS + ptrs_per_block + ptrs_per_block * ptrs_per_block;

    // SMP-safe per-call scratch (block_buf_ is shared/non-thread-safe). Heap
    // not stack: file_write can run deep in the demand-page path (IST2=4KB).
    KmBuf scratch(4096);
    if (!scratch) {
        return cinux::lib::Error::IOError;
    }

    auto*    src           = static_cast<const uint8_t*>(buf);
    uint64_t total_written = 0;

    while (total_written < count) {
        uint64_t file_block   = (offset + total_written) / bs;
        uint64_t block_offset = (offset + total_written) % bs;
        uint64_t chunk        = bs - block_offset;
        if (chunk > count - total_written) {
            chunk = count - total_written;
        }

        if (file_block >= max_file_block) {
            break;
        }

        uint32_t disk_block = ext2_.get_or_alloc_block(disk, static_cast<uint32_t>(file_block));
        if (disk_block == 0) {
            cinux::lib::kprintf("[EXT2] file_write: failed to alloc block for file_block %u\n",
                                static_cast<uint32_t>(file_block));
            break;
        }

        if (block_offset != 0 || chunk != bs) {
            if (!ext2_.read_block(disk_block, scratch.get())) {
                break;
            }
        } else {
            auto* dma = scratch.data();
            for (uint32_t i = 0; i < bs; ++i) {
                dma[i] = 0;
            }
        }

        auto* dst = scratch.data() + block_offset;
        for (uint64_t i = 0; i < chunk; ++i) {
            dst[i] = src[total_written + i];
        }

        if (!ext2_.write_block(disk_block, scratch.get())) {
            break;
        }

        total_written += chunk;
    }

    if (total_written > 0) {
        uint64_t new_end = offset + total_written;
        if (new_end > disk.i_size) {
            disk.i_size = static_cast<uint32_t>(new_end);

            uint32_t sectors_used = ((disk.i_size + bs - 1) / bs) * (bs / 512);
            disk.i_blocks         = sectors_used;
        }

        ext2_.write_disk_inode(static_cast<uint32_t>(inode->ino), disk);

        inode->size = disk.i_size;
    }

    // b4-bug4: write() bypasses the page cache (writes disk blocks directly),
    // so without invalidation a later mmap()/read() of the same file serves the
    // stale pre-write cached page (as wrote hello.o, ld then mmap'd it and saw
    // the zero-filled first page -> "file not recognized").  Refresh cached
    // pages overlapping this write in place so the next reader sees fresh bytes.
    if (total_written > 0) {
#ifndef CINUX_HOST_TEST
        cinux::mm::g_page_cache.invalidate_range(inode, offset, total_written);
#endif
    }

    return static_cast<int64_t>(total_written);
}

cinux::lib::ErrorOr<void> Ext2FileOps::truncate(Inode* inode, uint64_t new_size) {
    if (inode == nullptr || inode->fs_private == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    auto*      cached = ext2_cached_inode(inode);
    Ext2Inode& disk   = cached->disk_inode;

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

cinux::lib::ErrorOr<void> Ext2FileOps::stat(const Inode* inode, struct stat* st) {
    return ext2_.fill_stat(inode, st);
}

cinux::lib::ErrorOr<void> Ext2::fill_stat(const Inode* inode, struct stat* st) const {
    if (inode == nullptr || inode->fs_private == nullptr || st == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    const Ext2Inode& disk = ext2_cached_inode(inode)->disk_inode;
    // Zero first so the Linux-ABI fields the backend does not set (__pad0,
    // *_nsec, __unused) stay 0 -- no kernel-stack bytes leak to user space.
    memset(st, 0, sizeof(*st));
    st->st_dev     = 0;
    st->st_ino     = inode->ino;
    st->st_nlink   = disk.i_links_count;
    st->st_mode    = disk.i_mode;
    st->st_uid     = disk.i_uid;
    st->st_gid     = disk.i_gid;
    st->st_rdev    = 0;
    st->st_size    = disk.i_size;
    st->st_blksize = block_size();
    st->st_blocks  = disk.i_blocks;
    st->st_atime   = disk.i_atime;
    st->st_mtime   = disk.i_mtime;
    st->st_ctime   = disk.i_ctime;
    return {};
}

// ============================================================
// F-ECO batch 2 stubs (block A replaces these with real implementations).
// ============================================================

cinux::lib::ErrorOr<void> Ext2FileOps::chmod(Inode* inode, uint32_t mode) {
    if (inode == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    return ext2_.chmod(static_cast<uint32_t>(inode->ino), mode) ? cinux::lib::ErrorOr<void>{}
                                                                : cinux::lib::Error::IOError;
}

cinux::lib::ErrorOr<void> Ext2FileOps::chown(Inode* inode, uint32_t uid, uint32_t gid) {
    if (inode == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    return ext2_.chown(static_cast<uint32_t>(inode->ino), uid, gid) ? cinux::lib::ErrorOr<void>{}
                                                                    : cinux::lib::Error::IOError;
}

cinux::lib::ErrorOr<void> Ext2FileOps::utimensat(Inode* inode, uint64_t atime_sec,
                                                 uint32_t atime_nsec, uint64_t mtime_sec,
                                                 uint32_t mtime_nsec) {
    if (inode == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    return ext2_.utimensat(static_cast<uint32_t>(inode->ino), atime_sec, atime_nsec, mtime_sec,
                           mtime_nsec)
               ? cinux::lib::ErrorOr<void>{}
               : cinux::lib::Error::IOError;
}

cinux::lib::ErrorOr<int64_t> Ext2FileOps::readlink(const Inode* inode, char* buf,
                                                   uint64_t buf_size) {
    if (inode == nullptr || buf == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    int64_t n = ext2_.readlink(static_cast<uint32_t>(inode->ino), buf, buf_size);
    if (n < 0) {
        return cinux::lib::Error::IOError;
    }
    return n;
}

}  // namespace cinux::fs
