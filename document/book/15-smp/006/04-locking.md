---
title: 04 · 主线三 · 病灶上锁:inode_cache 套大锁,顺手清三笔旧债
---

# 主线三 · 病灶上锁:inode_cache 套大锁,顺手清三笔旧债

## 主线三 · 病灶上锁:inode_cache 套大锁,顺手清三笔旧债

### 主线:整段 walk/evict/insert 套进一把自旋锁

修法很直白——给 `inode_cache_` 配一把 `inode_cache_lock_`,在 `get_cached_inode` 入口拿锁 + 断言:

```cpp
Inode* Ext2::get_cached_inode(uint32_t ino) {
    // B3 inode cache race: inode_cache_ is shared across CPUs.  Serialize the
    // walk/evict/insert under inode_cache_lock_.  Held across read_disk_inode
    // (disk I/O) -- cache miss is rare and slow anyway; dropping the lock for
    // I/O would need a TOCTOU recheck.  lockdep_assert_held is the regression
    // guard if a future refactor drops the guard.
    auto g = inode_cache_lock_.guard();
    lockdep_assert_held(&inode_cache_lock_);
    if (ino == 0) { return nullptr; }
    // ... walk / evict / insert 全在锁内 ...
}
```

（[ext2_inode.cpp](../../../libs/ext2/ext2_inode.cpp#L93-L103)。注释把设计取舍讲透了。）锁成员声明挨在 cache 表旁边:

```cpp
Ext2CachedInode* inode_cache_[EXT2_INODE_CACHE_SIZE]{};
uint32_t inode_cache_count_{0};
mutable cinux::proc::Spinlock inode_cache_lock_;  ///< SMP: serialize cache walks/evicts
```

（[ext2.hpp](../../../libs/ext2/ext2.hpp#L491-L495)。结构(state)和并发(lock)两层加固同处一屏。)

#### 关键取舍:持锁跨 read_disk_inode 的盘 I/O

教科书会警告「别持自旋锁做 I/O」——持锁期间别的核要访问这张表就得 spin,盘 I/O 又慢。但这里合理,理由是 cache miss 本就是**稀有且慢**的路径:如果为了放锁去做「先占坑、放锁读盘、再加锁检查有没有别人插进来」的 TOCTOU 重检,会引入双倍复杂度(占坑标记、重检逻辑、谁负责读盘),收益不值。所以这里选择**正确性优先、锁覆盖全程**。这是 hobby 内核的典型取舍——先把正确性做扎实,性能等真成了瓶颈再说。

#### 两层护栏

加锁前用 race-detect 抓「根本没锁」;加锁后换成 `lockdep_assert_held` 防回归——万一未来有人重构把 `guard()` 那行删了,LOCKDEP 构建立刻 kpanic,bug 在测试阶段就炸,不会溜到生产。这是「同一缓存的两种检测器分工」的完整闭环。

#### 与结构层加固的关系:治结构 vs 治并发

`inode_cache_lock_` 不是第一层加固。在它之前,上一轮地基重做先把 inode cache 从「固定值数组」改成「堆分配 + 分离链 + refcount」:

```cpp
struct Ext2CachedInode {
    Ext2Inode        disk_inode;    ///< Copy of the on-disk inode
    Inode            vfs_inode;     ///< VFS-facing inode
    uint32_t         ino{0};        ///< Inode number …
    bool             stale{false};  ///< Disk inode changed under a live ref …
    Ext2CachedInode* hash_next{nullptr};  ///< Separate-chaining link …
    // …
};
```

（[ext2_types.hpp](../../../libs/ext2/ext2_types.hpp#L334-L340)。）那层治的是**结构性别名 UAF**——「slot 被驱逐重填导致活指针失效」。对象的地址即身份,只要 `refcount>0` 就绝不移动/重填,驱逐只挑 `refcount==0` 的:

```cpp
// evict:缓存满时只驱逐 refcount==0 的;全活则失败不腐蚀
if ((*pp)->vfs_inode.refcount == 0) { victim_prev = pp; break; }
// ...
if (victim_prev == nullptr) { return nullptr; }  // 全在用,失败也不重填活对象
```

（[ext2_inode.cpp](../../../libs/ext2/ext2_inode.cpp#L133-L150)。）但结构层加固只保证「单个 CPU 内、单线程语义下指针稳定」,没管「两个核同时进来改这张表」——那是这一章的活。**一个治结构(谁能在何时被释放),一个治并发(谁能同时进来改),缺一不可**。光有 refcount 不加锁,两核照样能同时 `new` + `read_disk_inode` + `insert`,重复读盘、重复挂桶;光有锁不保证地址即身份,驱逐重填照样让活指针失效。

### 顺手 rider:三个正确性债,两种结局

主线之外,这一轮顺手收了三笔正确性债。要诚实分开讲——两个修成了,一个没修成。

#### Rider ① NVMe io_submit —— 已落地

SMP 下两核同时往同一个 IO 队列塞命令,共享的 SQ tail / CQ head / phase 会被互相踩。修法是把「塞命令 + 等 completion」串进一把 `io_lock_` 的临界区:

```cpp
ErrorOr<uint16_t> NvmeController::io_submit(const NvmeCmd& cmd) {
    // SMP: serialize SQ enqueue + CQ poll -- io_sq_tail_/io_cq_head_/io_cq_phase_
    // are shared; two CPUs submitting at once clobber each other's sq slot and
    // mis-read completions (status=0x4080 on a legal LBA).
    io_lock_.acquire();
    // ... SQ enqueue + doorbell + 整个 CQ poll 循环 ...(末尾 io_lock_.release();)
}
```

（[nvme_io.cpp](../../../kernel/drivers/nvme/nvme_io.cpp#L20-L103)。`io_submit` 在 SMP 重构时拆出独立文件,锁用手动 `acquire()`/`release()` 包整段而非 RAII guard——因为循环中途有 yield 重入点。注释写明 race 表现:合法 LBA 读到 `status=0x4080`。）对照一下:`admin_submit` 无锁——它在 init 期单线程跑,不存在并发。

#### Rider ② ELF 加载校验 —— 已落地

损坏或恶意 ELF 头能让地址算术溢出回绕成小地址、或逼内核 alloc 几 MB 的 phdr 表。两道护栏:

```cpp
// validate_load_segment:三处 __builtin_add_overflow 截住地址回绕
if (__builtin_add_overflow(base, phdr.p_vaddr, &seg_vaddr) ||
    __builtin_add_overflow(seg_vaddr, phdr.p_memsz, &seg_memsz_end) ||
    __builtin_add_overflow(phdr.p_offset, phdr.p_filesz, &file_off_end)) {
    return ExecveResult::BadElfHeaders;
}
constexpr uint64_t kUserVaTop = 0x800000000000ULL;   // canonical user VA 上界兜底
if (seg_vaddr >= kUserVaTop || seg_memsz_end > kUserVaTop) {
    return ExecveResult::BadElfHeaders;
}
```

（[elf_load.cpp](../../../kernel/proc/elf_load.cpp#L41-L59)。GCC 没有 unsigned overflow 的 sanitize,只能靠 `__builtin_*_overflow`。）配套给 `e_phnum` 加上限,挡住损坏 ELF 逼内核 alloc + read ~3.6MB phdr 表:

```cpp
constexpr uint16_t kMaxPhnum = 256;   // real ELFs <30,256 是工程经验值不是规范值
if (ehdr->e_phnum > kMaxPhnum) { return ElfValidateResult::BadPhnum; }
```

（[elf_types.cpp](../../../kernel/proc/elf_types.cpp#L63-L72)。）

#### Rider ③ VFS offset_lock —— 已落地(分流锁)

这一笔是 schedule-while-held 这类 LOCKDEP 头号死锁模式的根治。病根曾经是:`do_read_kernel` / `do_write_kernel` 无条件持 `file->offset_lock_` 再调 `read` / `write`,而非 cacheable 路径(pipe / pty)的 `read` 会 `schedule_blocked` 让出 CPU——持着自旋锁去 schedule。

修法是用 `is_page_cacheable()` 把锁分流:只有 disk 路径(走 PageCache + demand page + NVMe poll,不阻塞)才持 `offset_lock_` 并更新 offset;pipe / pty 这类会 `schedule_blocked` 的流式路径不持锁、unlocked 更新 offset:

```cpp
int64_t do_read_kernel(int fd, void* kbuf, uint64_t count) {
    // offset_lock_ guards file->offset (seek position).  Only disk-backed
    // (page_cacheable) files use offset; their read path (PageCache + demand
    // page + NVMe poll) does not block on schedule.  Pipes/pty are streams --
    // their read() calls schedule_blocked, so holding offset_lock_ across it
    // would deadlock (LOCKDEP: schedule-while-held).
    if (file->inode->ops->is_page_cacheable()) {
        auto g           = file->offset_lock_.guard();   // disk 路径才持锁
        auto read_result = cinux::mm::g_page_cache.read_bytes(...);
        // ... 更新 file->offset ...
    } else {
        // pipe/pty:unlocked 更新 offset,read() 内部可安全 schedule_blocked
    }
}
```

（[sys_read.cpp](../../../kernel/syscall/sys_read.cpp#L48-L57)。`sys_write.cpp:53` 同样已分流。）这条修法对应 CinuxOS 上游(提交 `f40bed1`),已回迁到 Book 工作树。

配套对比点很值得记住——`sys_lseek` 持 `offset_lock_` 改 offset 是对的([sys_lseek.cpp](../../../kernel/syscall/sys_lseek.cpp#L34-L35)):它做的是纯算术、不阻塞,不触发 schedule-while-held;NVMe `io_lock_` 跨 busy-wait poll 也是安全的(poll 不让出 CPU)——错的是「持着它去 `schedule_blocked`」,而分流锁正是把这一刀切干净。
