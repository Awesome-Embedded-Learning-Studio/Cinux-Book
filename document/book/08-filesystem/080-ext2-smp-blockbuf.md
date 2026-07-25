---
title: 080 · ext2 独立成库——把共享 buffer 治成 SMP-safe
---

# 080 · ext2 独立成库——把共享 buffer 治成 SMP-safe

> 078、079 把 VFS 的地基和收尾都做了:组件遍历、引用计数、inode 缓存、dentry cache、flock。可底下那个最大的具体文件系统——ext2——一直带着一道老伤疤在跑。它在每个 `Ext2` 实例上挂了一块 4KB 的 `block_buf_[4096]` 共享 scratch,所有「块级」读写(读 indirect 指针、读 bitmap、写目录块)都要从这块缓冲过水。单核测试从来没事;可一旦两个 CPU 同时压上去——一边 demand page 读、一边 bitmap 分配——这块共享 buffer 就被来回覆盖,崩点离病灶隔了好几层调用,QEMU forensics 都抓不准。
>
> 这一章干两件缠绕在一起的事。第一件是把 ext2 从 `kernel/fs/ext2/` 搬到 `libs/ext2/` 成独立库——搬家本身不是为了好看,是为了**能在 host 上脱开 QEMU 直接跑 ext2 的真逻辑、上 TSAN 这种确定性竞态工具**。第二件是顺这条搬家绳,把 `block_buf_` 治成 per-call `KmBuf`——每条 SMP 路径自带一块独立 buffer,谁也不踩谁。搬家是表,治 race 是里。
>
> 这一章咱们还得把两种病灶分清:078 讲的是「静默别名」——那是**数据结构 race**(`inode_cache_` 里活引用被驱逐覆盖),治法是引用计数;这一章讲的是「共享 scratch clobber」——那是**中间 buffer 被并发覆盖**,加锁/引用计数都治不了,治法只能是消除共享。两类 race 长得像,病根正交,别混。

## 这章咱们要点亮什么

1. **能跑的 ext2 ≠ SMP-safe 的 ext2**:`block_buf_` 在单核下是合理的单所有者用法,可它挂在**实例**上——同一个 ext2 实例的所有块 I/O 共享它。SMP 一压就互踩,而且症状(wild 块号)离病灶(buffer 被覆盖)隔了好几层,单靠 QEMU forensics 难抓。搬家到 host、上 TSAN,是为了有「第一秒就抓到」的确定性工具。
2. **共享 scratch clobber 是单独一类 race,治法是消除共享不是加锁**:这是和 078「静默别名」最关键的区别。引用计数治「对象被释放」,加锁治「读改写 RMW 的并发改」,两者都治不了「两个调用方往同一块中间 buffer 写」。只有给每条 SMP 路径一块**自己的 buffer** 才行。
3. **4KB 的 buffer 不能放栈,不是偏好,是约束**:`#PF` 跑在受限栈上(它可能嵌在中断上下文、甚至嵌在别的 ISR 里触发,可用栈深本来就宝贵;文件读写调用链还会深递归进 demand-page,把 16KB 任务栈也吃紧),在它里面再撑一个 4KB 栈数组就贴着栈底跑了。所以选堆——`KmBuf` 一个极简 RAII 类,构造 `kmalloc`、析构 `kfree`、`operator bool` 查 OOM。约束选容器,不是偏好选容器。
4. **大调用面的 race 修复,标准姿势是加重载不是改签名**:`read_block(blk)` / `write_block(blk)` 这些单参版**保留**——它们挂在 init 期(挂载时单线程读 superblock/bgdt,那时还没起并发,加注释证安全);新增 `read_block(blk, dst)` / `write_block(blk, src)` 双参版,SMP 路径全走这组。不删旧签名、给新签名明确标注,比一刀切干净更诚实也更安全。

## 主线一:病灶——`block_buf_` 共享 scratch,SMP clobber → wild 块号 → segfault

光说「会 race」太虚,咱们拆一个具体崩点。

`Ext2` 实例上有这么一个成员:

```cpp
/// Scratch block buffer for read_block()/write_block() (max ext2 block = 4096 B)
uint8_t block_buf_[4096];
```

([ext2.hpp:444-445](../../../libs/ext2/ext2.hpp#L444))所有「块级」I/O——读 indirect 指针数组、读 bitmap、写目录块——都要经它过水。`read_block(blk)` 的实现就是往 `block_buf_` 里灌:

```cpp
bool Ext2::read_block(uint32_t block_num) {
    return read_block(block_num, block_buf_);  // shared-buffer variant (NOT SMP-safe)
}
```

([ext2_init.cpp:47-49](../../../libs/ext2/ext2_init.cpp#L47))

现在想象两 CPU 同时跑:

- **CPU A** 走 demand page read,要解析某个文件的 indirect 指针。它进 `resolve_disk_block_`,把 indirect 块 `read_block` 灌进 `block_buf_`,准备读 `indirect[i]` 拿数据块号;
- **CPU B** 这时要分配一个新块,走 `alloc_block` 的 bitmap RMW,也 `read_block` 把 bitmap 块灌进**同一个 `block_buf_`**,把 A 的 indirect 数据覆盖成了 bitmap 字节;
- **CPU A** 回来读 `indirect[i]`,读到的不是 indirect 指针而是 bitmap 的某个字节——这串字节被当成块号,很可能是个**远大于卷块数的 wild block 号**;
- wild block 号交给 NVMe,`dev_->read_blocks` 越界,`[EXT2] read_block(N) I/O failed`;demand page 拿不到数据,`#PF` 收不了尾 → segfault。

这条因果链上,崩点(`read_block(N) I/O failed` 的那条日志)和病灶(`block_buf_` 被 CPU B 覆盖)隔了 `resolve_disk_block_` → `read_block` → `dev_->read_blocks` 好几层调用。在 QEMU 里看到 segfault,回头翻日志,大概率只会怀疑 NVMe 驱动或者 bitmap 逻辑——猜一圈都猜不到头顶那块共享 buffer。头一回撞这种崩点,往往也是在 bitmap 逻辑和 NVMe 驱动里转半天,压根不往头顶那块共享 buffer 上想;真正能把思路掰过来的,是把它搬到 host 上让 TSAN 直接喊出「线程 A 读、线程 B 写同一块内存」那一刻。这正是源码里那条注释想拦住的东西:

```cpp
// SMP-safe per-call scratch (block_buf_ is shared/non-thread-safe; two CPUs
// demand-page-reading would clobber each other -> wild block numbers). Heap
// not stack: #PF runs on IST2 which is only 4 KB (IRQ_STACK_PAGES=1).
```

([ext2_common.cpp:94-96](../../../libs/ext2/ext2_common.cpp#L94))

三件事必须讲清:

1. **这是「共享 scratch clobber」型 race,不是数据结构 race**。加锁治不了——给 `block_buf_` 加一把锁让两 CPU 排队用行不通,因为 `resolve_disk_block_` 读 indirect 的过程本身就是「读一块、解析它、再读下一块」的多步序列,中间夹着别的逻辑,全程持锁代价太大、也容易死锁。引用计数也治不了——这块 buffer 不是被「释放」,是被「合法地写别的数据」覆盖。唯一根治是**消除共享**:每条 SMP 路径自己带一块 buffer。
2. **它是隐性的**。单核测试永远抓不到,只有 SMP 负载(demand page + bitmap alloc 并发)才暴露。078 那条 race-detect 红线提醒过这类——「单核绿不代表 SMP 绿」。
3. **QEMU forensics 容易漏**。崩点离病灶隔了好几层,得多走运才能在日志里把因果串起来。真正能秒抓的工具是 **TSAN**——它直接报「这块内存在线程 A 读、线程 B 写」。可上 TSAN 的前提是代码能在 **host 上脱开 QEMU 跑**——这正是「搬独立库」这步的动机回环。

## 主线二:治法 1——`KmBuf` RAII,为什么堆不栈

per-call buffer 的容器选什么?第一反应是「栈上 `uint8_t buf[4096]` 不就完了」。这里不行,原因是**栈深度约束**。

demand page 的 `#PF` 跑在受限栈上(它可能嵌在中断上下文、甚至嵌在别的 ISR 里被再入,可用栈深本来就宝贵;而且文件读写调用链会深递归进 demand-page,16KB 任务栈也吃紧)。源码注释把这条约束写在 `KmBuf` 的类文档里:

```cpp
/// RAII kmalloc'd scratch buffer for ext2 SMP read/write paths (block_buf_ is
/// shared/non-thread-safe). Heap not stack: demand-page #PF runs on IST2 which
/// is only 4 KB (IRQ_STACK_PAGES=1). ~KmBuf kfrees; operator bool checks OOM.
class KmBuf {
    void* p_;
public:
    explicit KmBuf(uint64_t n) : p_(cinux::mm::kmalloc(n, 16)) {}
    ~KmBuf() {
        if (p_ != nullptr) {
            cinux::mm::kfree(p_);
        }
    }
    KmBuf(const KmBuf&)            = delete;
    KmBuf& operator=(const KmBuf&) = delete;
    explicit operator bool() const { return p_ != nullptr; }
    void*    get() const { return p_; }
    uint8_t* data() const { return static_cast<uint8_t*>(p_); }
};
```

([ext2_common.hpp:24-41](../../../libs/ext2/ext2_common.hpp#L24))

在 `#PF` 里再撑一个 4KB 的栈数组就贴着栈底跑了;就算不在 `#PF` 里,文件读写的调用链也会深递归进 demand-page 路径,把 8KB 任务栈(`TaskBuilder::STACK_PAGES = 2`、AP 内核栈 `kStackPages = 4`,都见 `task_builder.hpp` / `ap_main.cpp`)也吃紧。所以选堆。

> **关于那条源码注释的常量名**:注释里写的是「`#PF runs on IST2 which is only 4 KB (IRQ_STACK_PAGES=1)`」。这个写法在「`#PF` 走不走 IST」上要分清——`kernel/arch/x86_64/idt.cpp` 的 IDT 路由表里 `#PF`(`ExceptionVector::PF`)的 `ist = 0`,即 `#PF` 不走 IST、跑在主任务栈上;而 IST2 那 4KB(`IRQ_STACK_PAGES = 1`,见 `gdt.hpp:115-119`)是给**硬件 IRQ**用的,不是给 `#PF`。所以注释把 `#PF` 跟 IST2 混着说,措辞不准。但**结论本身成立**——`#PF` 跑在主任务栈上(`ist = 0`),栈深受限、调用链还深,4KB buffer 不该再压栈。咱们这里把注释当动机引用,选堆的真正理由是上面那段「调用链深 + 栈余量宝贵」,而不是字面上那 4KB 的 IST。

`KmBuf` 刻意做小,就这四个方法:

- 构造 `kmalloc(n, 16)`——按 16 对齐(ext2 块结构体里很多字段需要 `uint32_t` 对齐,块大小 4096 也是 16 的倍数)。
- 析构 `kfree`——这是 RAII 的核心:调用方在自己栈帧上 `KmBuf scratch(4096);`,出作用域编译器自动 `kfree`,既消除共享又不用记着 free。
- `operator bool` 查 OOM——`kmalloc` 在内存紧时可能失败,调用方**必须检查**,不能假设成功就往里写(否则就是空指针写)。整个库里每个 `KmBuf` 的调用点后面都跟着 `if (!scratch) return ...;`。
- `get()` / `data()` 取指针。

它**不是 smart pointer**,不引入所有权复杂度,不 share、不 move、不 count ref。它就是一个一次性 scratch 载体,构造、用、析构,完事。范围小是刻意的——这个场景不需要更多。

## 主线三:治法 2——双参 SMP-safe 重载,单参 `block_buf_` 留 init 期

API 怎么改才能既治 race、又不一次性砸掉所有调用方?做法是**加重载不是改签名**:

```cpp
/// Read into block_buf_ (NOT SMP-safe; see dst overload).  @return true on success.
bool read_block(uint32_t block_num);
/// SMP-safe: read straight into @p dst (caller-provided).
bool read_block(uint32_t block_num, void* dst);
```

([ext2.hpp:120-123](../../../libs/ext2/ext2.hpp#L120))

```cpp
/**
 * ...  NOT SMP-safe (block_buf_); SMP paths use write_block(b, src).
 */
bool write_block(uint32_t block_num);
/// SMP-safe: write @p src straight to disk (caller-provided).
bool write_block(uint32_t block_num, void* src);
```

([ext2.hpp:129-139](../../../libs/ext2/ext2.hpp#L129))

```cpp
/// Zero block_buf_ then write to @p blk.  NOT SMP-safe; SMP uses the src overload.
bool zero_and_write_block(uint32_t blk);
/// SMP-safe: zero @p src then write it.
bool zero_and_write_block(uint32_t blk, void* src);
```

([ext2.hpp:141-144](../../../libs/ext2/ext2.hpp#L141))

三个块 I/O 操作各加一个双参版,buffer 由调用方提供;**SMP 路径全走这组双参版**。单参版保留,内部仍用 `block_buf_`:

```cpp
bool Ext2::read_block(uint32_t block_num) {
    return read_block(block_num, block_buf_);  // shared-buffer variant (NOT SMP-safe)
}
```

([ext2_init.cpp:47-49](../../../libs/ext2/ext2_init.cpp#L47))`write_block` / `zero_and_write_block` 的单参版同理(见 [ext2_init.cpp:73-89](../../../libs/ext2/ext2_init.cpp#L73))。

为什么不直接删单参版?因为 init 期(挂载时读 superblock、BGDT)还在用它们,而且**用得合理**——那时单线程、还没起并发,`block_buf_` 是货真价实的「单所有者」:

```cpp
// Mount-only, single-threaded use of the shared scratch buffer.
// Read the superblock (byte offset 1024 = LBA 2, 2 sectors)
...
if (!dev_->read_blocks(SB_LBA, SB_SECTORS, block_buf_).ok()) { ... }
memcpy(&sb_, block_buf_, sizeof(Ext2Superblock));
```

([ext2_init.cpp:127-137](../../../libs/ext2/ext2_init.cpp#L127))

```cpp
// Read the block group descriptor table. mount() is single-threaded, so
// the shared block_buf_ and no-dst read_block() overload are safe here.
...
    if (!read_block(bgdt_block + i)) { ... }
    auto* src = block_buf_;
```

([ext2_init.cpp:165-181](../../../libs/ext2/ext2_init.cpp#L165))

把这些 init 期的单线程用法也强改成 `KmBuf` 不是不行,但徒增改动面和回归风险——它们本来就是安全的。留它、并在头文件注释里**明确标注**「`NOT SMP-safe`; SMP uses the dst/src overload」(三个单参版的文档分别在 [ext2.hpp:120](../../../libs/ext2/ext2.hpp#L120) / [ext2.hpp:129](../../../libs/ext2/ext2.hpp#L129) / [ext2.hpp:141](../../../libs/ext2/ext2.hpp#L141)),比删干净更诚实:把「这块代码只能单线程用」明明白白写进注释,而不是删掉信号、留一个看起来人畜无害实则只能在特定时序下用的接口。

> **加重载而不是改签名,是内核这种大调用面代码做 race 修复的标准姿势**。改签名会让所有调用点一次性编译失败,得在同一个 commit 里把几十处全改对——漏一处就是编译错,而且分不清「哪些点是 SMP 路径、哪些是 init 期」。加重载则把决策**分散到每个调用点**:每个调用方自己决定走哪版,SMP 的换双参、init 的留单参。改动可逐个落地、可逐个 review,这就是安全演进。

## 主线四:治法 3——调用方迁移,把每条 SMP 路径换成 per-call `KmBuf`

API 给了,接下来是体力活——逐个把 SMP 路径上的调用方迁到双参版 + 自带 `KmBuf`。这是最容易漏的地方,得讲清每个调用方迁了之后消除了哪个具体 clobber 点。

**`resolve_disk_block_`——读 indirect 指针数组那条最敏感的路径**(主线一里 wild 块号的直接来源)。它原本 clobber `block_buf_`,现在加一个 `uint8_t* scratch` 参数,由上层 `Ext2FileOps::read` 传进来:

```cpp
uint32_t Ext2FileOps::resolve_disk_block_(const Ext2Inode& disk, uint64_t file_block,
                                          uint64_t block_ptrs_per_block, uint8_t* scratch) {
    ...
    if (!ext2_.read_block(indirect_block, scratch)) return 0;
    const auto* indirect = reinterpret_cast<const uint32_t*>(scratch);
    uint32_t    blk      = indirect[file_block - EXT2_DIRECT_BLOCKS];
```

([ext2_common.cpp:196-219](../../../libs/ext2/ext2_common.cpp#L196))double-indirect 那段([ext2_common.cpp:221-238](../../../libs/ext2/ext2_common.cpp#L221))同理,二级都用同一个 `scratch`——因为这条解析路径上「读完一级解析完再读下一级」,是顺序的,一块 buffer 够。

上层 `read()` 自己 new 一块 `KmBuf`,出函数自动释放:

```cpp
// SMP-safe per-call scratch ...
KmBuf scratch(4096);
if (!scratch) {
    return cinux::lib::Error::IOError;  // slab OOM
}
...
uint32_t disk_block = resolve_disk_block_(disk, file_block, block_ptrs_per_block, scratch.data());
```

([ext2_common.cpp:97-113](../../../libs/ext2/ext2_common.cpp#L97))

**`get_or_alloc_block`——独立 `zbuf`,不再「写完重读」覆盖父 array**。这条路径上要给新分配的块清零并写盘。老代码用 `block_buf_`,清零那一下就把刚读进来的 indirect 数组覆盖了,所以老逻辑只能「写完再重读父数组」;现在给清零一块独立 `zbuf`,父 array 的 `buf` 原封不动:

```cpp
KmBuf zbuf(4096);
if (!zbuf || !zero_and_write_block(data_blk, zbuf.get())) { free_block(data_blk); return 0; }
// zbuf was a separate buffer, so child_ptrs (the child array in child_buf)
// is still intact -- patch and write.
child_ptrs[idx2] = data_blk;
if (!write_block(child_blk, child_buf.get())) { ... }
```

([ext2_inode.cpp:446-453](../../../libs/ext2/ext2_inode.cpp#L446))。源码注释把这点写得很直白([ext2_inode.cpp:376-381](../../../libs/ext2/ext2_inode.cpp#L376)):「the old shared-block_buf_ code had to re-read the parent each time because zeroing the child clobbered the only buffer」。迁完之后 double-indirect 的两层 walk 各自一块 `KmBuf`(`di_buf` / `child_buf`),互不踩。

**`read_disk_inode` / `write_disk_inode`——各自 `KmBuf`,顺手把 `locate_inode_block` 改成「只算术不读」**。这俩是 RMW 风格(读 inode 所在块、patch inode 槽、写回),原来共用 `block_buf_`,现在各自一块:

```cpp
bool Ext2::read_disk_inode(uint32_t ino, Ext2Inode& out_inode) {
    InodeLoc loc{};
    if (!locate_inode_block(ino, loc)) { return false; }
    KmBuf buf(4096);
    if (!buf) { return false; }
    if (!read_block(loc.target_block, buf.get())) { ... }
    memcpy(&out_inode, buf.data() + loc.within_block_offset, sizeof(Ext2Inode));
    return true;
}
```

([ext2_inode.cpp:48-63](../../../libs/ext2/ext2_inode.cpp#L48))`write_disk_inode` 同款([ext2_inode.cpp:65-87](../../../libs/ext2/ext2_inode.cpp#L65))。

同时,`locate_inode_block` 从「定位 + `read_block` 它」改成**只做算术定位**(算 `target_block` 和 `within_block_offset`),把「读块」交给上层各自的 `KmBuf`:

```cpp
// Pure arithmetic bounds check; the block read is the caller's job
// (read_disk_inode / write_disk_inode each use their own KmBuf so two
// CPUs touching different inodes don't share block_buf_).
```

([ext2_inode.cpp:38-40](../../../libs/ext2/ext2_inode.cpp#L38))。这一改直接去掉一处共享读写——定位逻辑本来就不需要真读块,读是上层的活,让它显式地用自己的 buffer 读。

**bitmap alloc/free** —— 每次进 `alloc_block` / `free_block` 自己 `KmBuf`:

```cpp
KmBuf blk_buf(4096);
if (!blk_buf || !read_block(bitmap_block, blk_buf.get())) { ... }
auto* bitmap = blk_buf.data();
```

([ext2_block.cpp:43-50](../../../libs/ext2/ext2_block.cpp#L43),`free_block` 见 [ext2_block.cpp:124-128](../../../libs/ext2/ext2_block.cpp#L124))。

**`Ext2FileOps::read`** —— 用 `scratch.data()`(主线三已贴)。**directory readdir** —— `Ext2DirOps::readdir` 在 `ext2_dirops.cpp` 里,每个目录块自己 `KmBuf buf(4096)`([ext2_dirops.cpp:73](../../../libs/ext2/ext2_dirops.cpp#L73))。同理,目录写路径 `add_dir_entry`(在另一个文件 `ext2_directory.cpp`)也是每个目录块一块独立 `KmBuf dir_buf`([ext2_directory.cpp:43-50](../../../libs/ext2/ext2_directory.cpp#L43)),逻辑一致。

迁移策略不是一把梭,是**按路径逐个替换 + 每个 `grep` 确认 `block_buf_` 不再出现在 SMP 路径**——这跟 lab 的 grep 验收呼应。最后一公里的风险要讲明:漏一个调用点就是留一个 race。所以 lab 不让读者改代码,而是让读者自己 `grep` 验收无残留——把迁移纪律内化成「能自己判断 block_buf_ 还该出现在哪儿」。

## 主线五:配套纪律 1——unlink 的 indirect 快照

迁移之外,还有一个由「`block_buf_` 共享」派生出的隐患,治法不是 `KmBuf` 而是**快照**。

场景:`unlink` 释放大文件的间接块时,要遍历 indirect 指针数组去 free 每个数据块。可 `free_block()` 自己会 `read_block(bitmap) + write_block(bitmap)`——bitmap 块也走 `block_buf_`(老逻辑下)。如果不快照,free 第一个数据块时,`free_block` 的 bitmap RMW 就把 `block_buf_` 里的 indirect 数组覆盖成了 bitmap 字节,回头再读 `indirect[1..]` 读到的就是 bitmap 字节、被当块号——「group out of range」的垃圾块号,free 到根本不存在的块上。

治法:先把整个 indirect 指针数组 `read_block` 进**实例级快照缓冲**,再开始 free 数据块:

```cpp
/// Snapshot buffers for unlink()'s indirect-block release.  free_block()
/// does its own read_block(bitmap)+write_block(bitmap), which overwrite
/// block_buf_; so unlink must copy each indirect pointer array out of
/// block_buf_ BEFORE freeing the data blocks it lists -- otherwise every
/// entry after the first free_block() reads bitmap bytes reinterpreted as
/// a block number (the "group out of range" garbage seen when a file that
/// spans indirect blocks is unlinked).  Two buffers because the doubly-
/// indirect walk is nested: the top-level array must survive while each
/// child's array is processed, so they cannot share one buffer.
uint32_t unlink_ptr_buf_[1024];
uint32_t unlink_child_buf_[1024];
```

([ext2.hpp:447-457](../../../libs/ext2/ext2.hpp#L447))

single-indirect 的释放:

```cpp
// Free singly-indirect block and its referenced data blocks.
// SNAPSHOT discipline: read the indirect pointer array straight into
// unlink_ptr_buf_ (SMP-safe dst overload) BEFORE freeing the data
// blocks it lists.  free_block() does its own read/write of the bitmap
// block; snapshotting the array first keeps every entry intact across
// those later I/Os ...
if (read_block(indirect_blk, unlink_ptr_buf_)) {
    for (uint32_t i = 0; i < ptrs_per_block; ++i) {
        if (unlink_ptr_buf_[i] != 0) {
            free_block(unlink_ptr_buf_[i]);
        }
    }
}
```

([ext2_directory.cpp:402-424](../../../libs/ext2/ext2_directory.cpp#L402))

double-indirect 是嵌套 walk:外层数组得在「处理每个 child 数组」的整个过程中存活,所以**两个快照缓冲**——`unlink_ptr_buf_` 装顶层、`unlink_child_buf_` 装每个 child:

```cpp
// Two-level snapshot: unlink_ptr_buf_ holds the top-level array across the
// outer loop; unlink_child_buf_ holds each child's array inside the inner loop
// (they cannot share a buffer -- the outer array must survive inner processing).
if (read_block(di_blk, unlink_ptr_buf_)) {
    for (uint32_t i = 0; i < ptrs_per_block; ++i) {
        uint32_t child_blk = unlink_ptr_buf_[i];
        if (child_blk == 0) { continue; }
        if (read_block(child_blk, unlink_child_buf_)) {
            for (uint32_t j = 0; j < ptrs_per_block; ++j) {
                if (unlink_child_buf_[j] != 0) {
                    free_block(unlink_child_buf_[j]);
                }
            }
        }
        free_block(child_blk);
    }
}
```

([ext2_directory.cpp:426-457](../../../libs/ext2/ext2_directory.cpp#L426))

这里要诚实说一个**还没收尾的口子**。`unlink_ptr_buf_` / `unlink_child_buf_` 这两块快照是**实例级共享**的——它和 `block_buf_` 同病。快照治的是「同一次 unlink 内部,free 数据块的过程会 clobber 正在遍历的 indirect 数组」这一层 clobber(这是本章关心的、已经治住的那一类);但它**不治**「两个 CPU 同时对同一 ext2 实例上不同路径的文件并发 unlink」这一层——核对 `sys_unlink.cpp`,从 `parent->ops->unlink(...)` 进来到 `Ext2::unlink` 遍历 indirect,全程没有 per-inode / per-fs 的锁把 unlink 串行化;`Ext2::unlink` 自身在遍历这两块快照时也不持 `inode_cache_lock_` 或 `block_alloc_lock_`(`block_alloc_lock_` 只在 `free_block` 内部保护 bitmap RMW,覆盖不到快照缓冲)。也就是说:跨 inode 的并发 unlink 会让两个 CPU 同时读写这两块实例级快照,留下一个真实的残留 race。

这是和「没搭 host TSAN 回归」并列的已知缺口——本章把「单次 unlink 内部的 snapshot clobber」治干净了(对应 `free_block` 的 bitmap RMW 不再覆盖 indirect 数组),但「跨 unlink 的快照缓冲共享」没收。诚实的收法是在 `Ext2::unlink` 入口加一把 per-instance `unlink_lock_`、把整个 indirect 释放串行化(类似 `block_alloc_lock_` 的做法),这一步留 follow-up,不在这章的 `block_buf_` 治理范围内。这章讲的「快照」纪律只覆盖前一层 clobber,别把它误读成「整条 unlink 路径已 SMP-safe」。

> **为什么这里用快照而不用 `KmBuf`?** 因为 free 数据块的过程本身是多次 read/write bitmap,中间不可能每次都重新读 indirect(indirect 在 free 完第一个块后可能本身也要被释放)。快照是「先把要遍历的东西固化下来再动手」这个通用并发纪律的具体实现——「边读边改同一份」这类隐患,通用招数就是先把要读的那份拷出来。

## 主线六:配套纪律 2——`block_alloc_lock_`,bitmap RMW 串行

第二类由「`block_buf_` 共享」之外的 SMP 隐患,治法是**锁**不是 buffer。

block/inode bitmap 的分配是 read-modify-write:读 bitmap 块、找 free bit、mark 它、写回。两 CPU 同时分配:都读到同一个 bit 是 free、都 mark 它、都写回——同一个块被分给两个文件,数据损坏(cc1 的 indirect 块被某个 `.o` 的写覆盖,就这么来的)。这跟 `block_buf_` clobber 无关,是 RMW 数据 race。

治法是 `block_alloc_lock_` 这个 `Spinlock` 成员:

```cpp
mutable cinux::proc::Spinlock block_alloc_lock_;  ///< SMP: serialize block+inode bitmap alloc/free
```

([ext2.hpp:496](../../../libs/ext2/ext2.hpp#L496))。`alloc_block` / `free_block` 进去先拿这把锁:

```cpp
uint32_t Ext2::alloc_block() {
    // SMP: bitmap read-modify-write (read bitmap, mark bit, write bitmap) is
    // not atomic -- two CPUs alloc concurrently can both see a bit free, both
    // mark+write, and return the SAME block to two files (one overwrites the
    // other's data, e.g. cc1's indirect block clobbered by a .o write).
    // Serialize under block_alloc_lock_.  Held across disk I/O (alloc is rare
    // relative to data read/write; cache miss is acceptable).
    auto g = block_alloc_lock_.guard();
    ...
```

([ext2_block.cpp:21-29](../../../libs/ext2/ext2_block.cpp#L21))`free_block` 同款([ext2_block.cpp:104-106](../../../libs/ext2/ext2_block.cpp#L104))。

**它和 `KmBuf` 的分工讲清楚**:

- `KmBuf` 治「**读写中间数据(scratch)被覆盖**」——`block_buf_` 这块 buffer 的内容被别的 I/O 冲掉;
- `block_alloc_lock_` 治「**读改写持久数据(bitmap)的 RMW race**」——bitmap 这份持久 metadata 被两 CPU 同时改。

一个治 scratch,一个治 metadata,两个正交,都要有。光上 `KmBuf` 不上锁,bitmap 还是会被分给两个文件;光上锁不上 `KmBuf`,indirect 数组还是会被 bitmap RMW 覆盖。

锁的**粒度**也值得讲一句:这是把整个 alloc/free 操作都串起来(粗粒度),不按 block group 细分。为什么?bitmap 修改是低频操作(分配/释放块远少于数据读写),而且必须串行才正确,不值得为了并发度去按 group 拆锁——**简单正确优先**。

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

## 主线八:host PAL——让 ext2 在 host 上脱开 QEMU,上 TSAN 抓 race

主线一开头咱们说过:QEMU forensics 漏掉的那一类 race,真正能秒抓的是 **TSAN**——它直接报「这块内存在线程 A 读、线程 B 写」。可上 TSAN 的前提是 ext2 能在 **host 上脱开 QEMU 直接跑**。搬家到 `libs/ext2/` 是表,让这一层成为可能才是底。这一节就把这条动机回环讲完:host PAL 怎么 mock 掉内核依赖、host 单测怎么验真逻辑、host 并发压测怎么让 TSAN 把 `block_buf_` race 喊出来。

### 为什么 ext2 能在 host 上跑——PAL 切了哪几刀

ext2 的源码(`libs/ext2/ext2_*.cpp`)在内核里编一份(`-mcmodel=kernel`),现在又**在 host 上编第二份**——直接用 host 的 codegen,跟 `net_tcp` 那条 host 测试同一个路子。可 ext2 调了几个只有内核才有的符号,host 上没有:

- `cinux::lib::kprintf` / `kvprintf` / `kpanic`:内核版走 serial + x86 inline asm(`outl` / `cli` / `hlt`),host 上没串口;
- `cinux::mm::kmalloc` / `kfree`:内核版路由到 PMM/buddy slab,host 上没有那套页管理。

host PAL 的活就是给这几个符号提供**libc 后端**,让 ext2 链得过、跑得动。`test/unit/ext2_host_pal.cpp` 是这块 PAL 的全部:

```cpp
namespace cinux::lib {

void kprintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);  // host: route to stdout
    va_end(args);
}

void kvprintf(const char* fmt, va_list args) {
    vprintf(fmt, args);
}

[[noreturn]] void kpanic(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    abort();
}

// Sink registration is a kernel multi-backend concept (serial + framebuffer +
// QEMU debugconsole). The host PAL has a single implicit stdout sink (the
// vprintf above), so these are no-ops.
void kprintf_register_sink(OutputSink /*fn*/, void* /*ctx*/) {}
void kprintf_set_sink_enabled(OutputSink /*fn*/, void* /*ctx*/, bool /*enabled*/) {}
void kprintf_enable_all_sinks() {}
void kprintf_init() {}

}  // namespace cinux::lib
```

([ext2_host_pal.cpp:29-58](../../../test/unit/ext2_host_pal.cpp#L29))`kprintf` 在 host 上就是 `vprintf` 往 stdout,kpanic 直接 `abort`。sink 注册那一套是内核的多后端(serial + framebuffer + debugconsole)概念,host 只有一个隐式 stdout sink,所以全是 no-op。

`kmalloc` / `kfree` 同理走 libc,但有一个**对齐 + 清零契约**要守:

```cpp
void* kmalloc(size_t size, size_t align) {
    if (size == 0) return nullptr;
    if (align < sizeof(void*)) align = sizeof(void*);
    // aligned_alloc requires size to be a multiple of alignment.
    size_t rounded = (size + align - 1) & ~(align - 1);
    void* p = aligned_alloc(align, rounded);
    // Match the kernel slab contract: returned memory is zeroed (no stale-data
    // leak), which ext2 scratch buffers (KmBuf) and the allocator paths assume.
    if (p != nullptr) {
        memset(p, 0, rounded);
    }
    return p;
}

void kfree(void* ptr) {
    free(ptr);
}
```

([ext2_host_pal.cpp:60-80](../../../test/unit/ext2_host_pal.cpp#L60))。注意那个 `memset(p, 0, rounded)`——内核 slab 返回的内存是清零的(无 stale-data 泄漏),ext2 的 `KmBuf` scratch buffer 和分配路径都依赖这点。host PAL 必须守这条契约,不然 host 上跑出来的行为跟内核里不一致,测出来的就没意义。

剩下两个符号来自别处、但同一个 target 里:`cinux::proc::Spinlock` 由 `host_spinlock.cpp` 提供(用 libc 的 `pthread_mutex` 实现 `Spinlock::guard`),`inode_ref` / `inode_unref` 由 `kernel/fs/file.cpp` 直接链进来(这俩是纯逻辑、host-safe,跟 devfs 测试一个先例)。这三个 PAL + 一个 ext2 库 = host 上能跑真 ext2 的全部材料。

### host 单测:真逻辑往返读写 + 硬链接

PAL 备齐,先验 ext2 在 host 上跑得对——`test/unit/test_ext2_host.cpp` 挂一张预构的 64 KiB ext2 镜像(`test/data/ext2_test.img`,由 `mke2fs -d` 预填 `/etc/motd` + `/hello.txt`)进一个 `RAMBlockDevice`,然后走**完整 VFS 路径**:

```cpp
Ext2 ext2(&dev);
ASSERT_OK(ext2.mount());

// --- readdir "/" : expect etc/ and hello.txt (index 0/1 are "." / "..") ---
auto root_r = ext2.lookup("/");
...
for (uint64_t i = 2; i < 64; ++i) {
    char nm[256];
    auto r = root->ops->readdir(root, i, nm, 255);
    ASSERT_TRUE(r.ok());
    if (*r == 0) break;  // end of directory
    if (std::strcmp(nm, "etc") == 0) found_etc = true;
    if (std::strcmp(nm, "hello.txt") == 0) found_hello = true;
}
ASSERT_TRUE(found_etc);
ASSERT_TRUE(found_hello);

// --- multi-level lookup + read /etc/motd ---
auto motd_r = ext2.lookup("/etc/motd");
...
    char buf[64];
    auto rd = motd->ops->read(motd, 0, buf, sizeof(buf) - 1);
    ASSERT_TRUE(rd.ok());
    ASSERT_GT(*rd, 0);
    buf[*rd] = '\0';
    ASSERT_TRUE(std::strstr(buf, "hello ext2 motd") != nullptr);
```

([test_ext2_host.cpp:58-88](../../../test/unit/test_ext2_host.cpp#L58))。注意它走的不是某个 mock 出来的接口,而是**真 ext2 的真 `Ext2::lookup` / 真 `InodeOps::read`**——`RAMBlockDevice` 只是替 NVMe/AHCI 把盘字节握在内存里,I/O 路径上别的逻辑全真的。这条测下来绿,说明 ext2 在 host 上跑得对:挂载、多层 lookup、readdir、read 都对得上镜像内容。

写路径也压——create + write + read-back + unlink 一条往返:

```cpp
// --- create /newfile + write + read-back (exercises block allocator) ---
auto newf_r = root->ops->create(root, "newfile", 7);
...
const char  payload[] = "hello-write";
const auto  plen = static_cast<int64_t>(sizeof(payload) - 1);
auto        wr = newf->ops->write(newf, 0, payload, static_cast<uint64_t>(plen));
ASSERT_TRUE(wr.ok());
ASSERT_EQ(*wr, plen);

char  rb[32];
auto  rb_r = newf->ops->read(newf, 0, rb, sizeof(rb) - 1);
ASSERT_TRUE(rb_r.ok());
ASSERT_EQ(*rb_r, plen);
rb[*rb_r] = '\0';
ASSERT_TRUE(std::strstr(rb, "hello-write") != nullptr);
```

([test_ext2_host.cpp:90-107](../../../test/unit/test_ext2_host.cpp#L90))。这一段最关键的是它**走了块分配器**——`create` 要分一个新 inode、`write` 要分一个新数据块,正是 `block_alloc_lock_` 要保护的那条 RMW 路径。在 host 上能跑通,说明新 ext2 的写+分配逻辑跟读路径一样,脱开 QEMU 也是对的。它跑在 ASAN 下,UAF / OOB / leak 都在毫秒级冒出来——这是 QEMU forensics 想要但抓不到的确定性。

> 这一节顺带说一个**先验链接**的测试:`test_ext2_host_link.cpp` 不验语义,只验「host PAL 备齐了」——构造一个 `Ext2` over `RAMBlockDevice`、调 `mount()`(零填充盘 → superblock magic 校验失败 → 返回 not ok)、析构。它链得过来、不崩,PAL 覆盖就完整了。这是 host 测试基建的「make-or-break」门槛,真语义留给上面那条 host 测。详见 [test_ext2_host_link.cpp:37-47](../../../test/unit/test_ext2_host_link.cpp#L37)。

### host 并发压测:TSAN 秒抓 `block_buf_` race

PAL 备齐、单线程真逻辑验对,接下来才是这条搬家绳真正的 payoff——**让 TSAN 把 `block_buf_` 这一类 race 喊出来**。`test/unit/test_ext2_concurrent.cpp` 起两个 stressor,每个都 N 线程压**同一个** `Ext2` + `RAMBlockDevice`:

**Stressor 1:并行 `alloc_block` / `free_block`,压 `block_alloc_lock_`**:

```cpp
// N threads each perform 100 alloc/free cycles on the SAME Ext2 instance. All
// bitmap / superblock / BGDT updates run under block_alloc_lock_; a missing
// guard shows up as a TSan data-race report on the bitmap bytes or free-count
// fields.
TEST("ext2_concurrent: parallel alloc_block / free_block (TSAN)") {
    ...
    constexpr int kThreads = 4;
    constexpr int kIters   = 100;
    std::thread   threads[kThreads];
    for (int t = 0; t < kThreads; ++t) {
        threads[t] = std::thread([&ext2]() {
            for (int i = 0; i < kIters; ++i) {
                uint32_t b = ext2.alloc_block();
                if (b != 0) {
                    ext2.free_block(b);
                }
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
}
```

([test_ext2_concurrent.cpp:51-83](../../../test/unit/test_ext2_concurrent.cpp#L51))。4 线程 × 100 次循环,每次都进 bitmap RMW。如果 `block_alloc_lock_` 漏了某处,TSAN 在几毫秒内就会报「bitmap 字节被线程 A 写、线程 B 读」——这就是主线六那把锁要拦的东西。

**Stressor 2:并行 `lookup("/etc/motd")`,压 `block_buf_` + `inode_cache_lock_`**:

```cpp
// Regression for the shared block_buf_ race found by the original M6 TSan run:
// lookup_in_dir() now owns one KmBuf per call and reuses it across directory
// blocks, so concurrent path resolution never shares scratch storage.
TEST("ext2_concurrent: parallel lookup (TSAN)") {
    ...
    constexpr int    kThreads = 4;
    constexpr int    kIters   = 200;
    std::atomic<int> failures{0};
    std::thread      threads[kThreads];
    for (int t = 0; t < kThreads; ++t) {
        threads[t] = std::thread([&ext2, &failures]() {
            for (int i = 0; i < kIters; ++i) {
                auto result = ext2.lookup("/etc/motd");
                if (!result.ok() || result.value() == nullptr) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                inode_unref(result.value());
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    ASSERT_EQ(failures.load(std::memory_order_relaxed), 0);
}
```

([test_ext2_concurrent.cpp:88-125](../../../test/unit/test_ext2_concurrent.cpp#L88))。这条是**直接回归 `block_buf_` race 的**——4 线程 × 200 次并发 `lookup`,每次 lookup 进 `lookup_in_dir` 都要 `read_block` 目录块。注释里写得很直白:「`lookup_in_dir()` now owns one KmBuf per call and reuses it across directory blocks, so concurrent path resolution never shares scratch storage」——也就是说,如果哪天有人把 `KmBuf` 改回共享 `block_buf_`,这条测在 TSAN 下会立刻报「`block_buf_` 在线程 A 读、线程 B 写」,正是主线一描述的那种 wild 块号 race 的根。这一条是 `block_buf_` 治理的**确定性回归证据**,不是 `run-kernel-test` 跑全绿那种概率证据。

构建开关也讲清:`-DCINUX_HOST_TSAN=ON` 给所有 host 测试加 `-fsanitize=thread`,跟 ASAN 互斥(见 `test/CMakeLists.txt:38-41`);`test_ext2_concurrent` 显式链 `-pthread`(`test/CMakeLists.txt:327`)。镜像路径由 CMake 注入:`EXT2_TEST_IMG_PATH="${CMAKE_SOURCE_DIR}/test/data/ext2_test.img"`([test/CMakeLists.txt:325-326](../../../test/CMakeLists.txt#L325))。

> **TSAN 和 lockdep 的分工**。内核里有 lockdep,但它只做 lock-order(锁序)检查,看不见「两个线程摸同一个字段、却没加锁」这种 race。TSAN 正好补这一块——它不需要你声明「这块内存归哪把锁管」,它直接看内存访问。所以 host 上跑 TSAN,是把内核 lockdep 看不见的那一类 race,在 host 上用确定性工具捞出来。这正是这一节存在的意义。

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

## 范围与边界(诚实说)

这一章的 race 治理有几条没收尾的口子,摊开讲清,别让读者读完以为「ext2 已 SMP-safe」。

- **host PAL + TSAN 确定性回归已到位**(原「没搭 host TSAN」那条 deferred,现已收)。主线八讲完:host PAL(`test/unit/ext2_host_pal.cpp`)mock 掉 `kprintf` / `kmalloc` 让 ext2 在 host 上跑真逻辑;`test_ext2_host.cpp` 走完整 VFS 往返(readdir + read + create/write/read-back + unlink);`test_ext2_concurrent.cpp` 4 线程压同一个 `Ext2`,`-DCINUX_HOST_TSAN=ON` 秒级抓 `block_buf_` race。`block_buf_` 治理现在是「逻辑根治 + 回归确定性验证」双重闭环,不再只是 `run-kernel-test` 全绿的概率证据。
- **`unlink` 的跨并发快照缓冲还没治**。主线五末尾已经诚实标注:`unlink_ptr_buf_` / `unlink_child_buf_` 治的是「单次 unlink 内部 free 过程的 clobber」,不治「两个 CPU 并发 unlink 同一 ext2 实例」那层共享快照 race——那层需要一把 per-instance `unlink_lock_`,留 follow-up。这章不假装它已 SMP-safe。
- **truncate 是 shrink-only,孤儿块不回收**。主线七已说,`O_TRUNC` 截断掉的孤儿数据块不释放,是已知 leak(hobby-os 式)。read 不超过 `i_size` 所以非正确性问题,只浪费磁盘;完整的孤儿块回收留 follow-up。
- **只讲「搬独立库 + 治 `block_buf_` 成 SMP-safe」这一根绳**。host PAL 的搭法主线八给了概貌(`kprintf` / `kmalloc` 走 libc + 守 slab 清零契约),但 PAL 设计的完整动机、ASAN/TSAN 开关的取舍不展开;`ext2_dirops.cpp` 这种「搬家顺带的结构整理」也只一句话带过,不教拆分方法论。
- **ext4 extent 读路径只到 depth-0 leaf**。主线九已讲:`resolve_disk_block_` 入口先判 `EXT4_EXTENTS_FL`,extent-mapped 的 inode 走 `extent_lookup_block` 那条 depth-0 leaf 解析(纯算术、不读盘、不碰 buffer,所以不在 `block_buf_` race 的范围内)。但 `eh_depth > 0` 的 index 节点(很大或很碎的文件才会用)`Unsupported`,本驱动 bail 不读——那一层会引入 index 块的 I/O、又得 `KmBuf`,留后续章。
- **目录读写路径(`lookup_in_dir` / symlink readlink 等)的 per-call `KmBuf`**已随这次搬家一起到位,本章按合并态讲,不展开每条目录路径的迁移细节。

> 这一章的 race 修复,`run-kernel-test` 跑全绿是验证证据(不是战绩)。它站得住的真正理由,是每条 SMP 路径都换了 per-call `KmBuf`、`grep` 确认 `block_buf_` 不再出现在并发路径上、配套的 `block_alloc_lock_` 串行了 bitmap RMW、unlink 的 indirect 快照隔离了 free 过程的 clobber——这一套逻辑是闭环的。再加上 host PAL 让 ext2 在 host 上跑真逻辑、`test_ext2_concurrent` 在 TSAN 下并发压 lookup(主线八),`block_buf_` race 有了确定性回归,不再只靠 QEMU 全绿。剩下没收的口子(unlink 跨并发快照、truncate 孤儿块、extent depth>0)都摊在上面,各自留了明确的 follow-up。
