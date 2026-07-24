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

([ext2.hpp:444-445](libs/ext2/ext2.hpp#L444))所有「块级」I/O——读 indirect 指针数组、读 bitmap、写目录块——都要经它过水。`read_block(blk)` 的实现就是往 `block_buf_` 里灌:

```cpp
bool Ext2::read_block(uint32_t block_num) {
    return read_block(block_num, block_buf_);  // shared-buffer variant (NOT SMP-safe)
}
```

([ext2_init.cpp:47-49](libs/ext2/ext2_init.cpp#L47))

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

([ext2_common.cpp:94-96](libs/ext2/ext2_common.cpp#L94))

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

([ext2_common.hpp:24-41](libs/ext2/ext2_common.hpp#L24))

在 `#PF` 里再撑一个 4KB 的栈数组就贴着栈底跑了;就算不在 `#PF` 里,文件读写的调用链也会深递归进 demand-page 路径,把 16KB 任务栈(`TaskBuilder::STACK_PAGES = 4`、AP 内核栈 `kStackPages = 4`,都见 `task_builder.hpp` / `ap_main.cpp`)也吃紧。所以选堆。

> **关于那条源码注释里的一处不准**:注释里写的是「`#PF runs on IST2 which is only 4 KB (IRQ_STACK_PAGES=1)`」。这条常量在本仓里其实对不上——`kernel/arch/x86_64/idt.cpp` 的 IDT 路由表里 `#PF`(vector 14)的 `ist = 0`,只有 `#DF`(Double Fault)走 IST1(`gdt.hpp` 里 `DF_STACK_PAGES = 1`),内核里既没有 IST2、也没有 `IRQ_STACK_PAGES` 这个符号。换句话说,注释把 `#PF` 错归到 IST2、还编了个不存在的常量名。但**结论本身成立**——`#PF` 跑在主任务栈上,栈深受限、调用链还深,4KB buffer 不该再压栈。咱们这里把注释当动机引用,不为注释里那两个常量名背书;选堆的真正理由是上面那段「调用链深 + 栈余量宝贵」,而不是字面上那 4KB 的 IST。

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

([ext2.hpp:120-123](libs/ext2/ext2.hpp#L120))

```cpp
/**
 * ...  NOT SMP-safe (block_buf_); SMP paths use write_block(b, src).
 */
bool write_block(uint32_t block_num);
/// SMP-safe: write @p src straight to disk (caller-provided).
bool write_block(uint32_t block_num, void* src);
```

([ext2.hpp:129-139](libs/ext2/ext2.hpp#L129))

```cpp
/// Zero block_buf_ then write to @p blk.  NOT SMP-safe; SMP uses the src overload.
bool zero_and_write_block(uint32_t blk);
/// SMP-safe: zero @p src then write it.
bool zero_and_write_block(uint32_t blk, void* src);
```

([ext2.hpp:141-144](libs/ext2/ext2.hpp#L141))

三个块 I/O 操作各加一个双参版,buffer 由调用方提供;**SMP 路径全走这组双参版**。单参版保留,内部仍用 `block_buf_`:

```cpp
bool Ext2::read_block(uint32_t block_num) {
    return read_block(block_num, block_buf_);  // shared-buffer variant (NOT SMP-safe)
}
```

([ext2_init.cpp:47-49](libs/ext2/ext2_init.cpp#L47))`write_block` / `zero_and_write_block` 的单参版同理(见 [ext2_init.cpp:73-89](libs/ext2/ext2_init.cpp#L73))。

为什么不直接删单参版?因为 init 期(挂载时读 superblock、BGDT)还在用它们,而且**用得合理**——那时单线程、还没起并发,`block_buf_` 是货真价实的「单所有者」:

```cpp
// Mount-only, single-threaded use of the shared scratch buffer.
// Read the superblock (byte offset 1024 = LBA 2, 2 sectors)
...
if (!dev_->read_blocks(SB_LBA, SB_SECTORS, block_buf_).ok()) { ... }
memcpy(&sb_, block_buf_, sizeof(Ext2Superblock));
```

([ext2_init.cpp:127-137](libs/ext2/ext2_init.cpp#L127))

```cpp
// Read the block group descriptor table. mount() is single-threaded, so
// the shared block_buf_ and no-dst read_block() overload are safe here.
...
    if (!read_block(bgdt_block + i)) { ... }
    auto* src = block_buf_;
```

([ext2_init.cpp:165-179](libs/ext2/ext2_init.cpp#L165))

把这些 init 期的单线程用法也强改成 `KmBuf` 不是不行,但徒增改动面和回归风险——它们本来就是安全的。留它、并在头文件注释里**明确标注**「`NOT SMP-safe`; SMP uses the dst/src overload」(三个单参版的文档分别在 [ext2.hpp:120](libs/ext2/ext2.hpp#L120) / [ext2.hpp:129](libs/ext2/ext2.hpp#L129) / [ext2.hpp:141](libs/ext2/ext2.hpp#L141)),比删干净更诚实:把「这块代码只能单线程用」明明白白写进注释,而不是删掉信号、留一个看起来人畜无害实则只能在特定时序下用的接口。

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

([ext2_common.cpp:196-219](libs/ext2/ext2_common.cpp#L196))double-indirect 那段([ext2_common.cpp:221-238](libs/ext2/ext2_common.cpp#L221))同理,二级都用同一个 `scratch`——因为这条解析路径上「读完一级解析完再读下一级」,是顺序的,一块 buffer 够。

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

([ext2_common.cpp:97-113](libs/ext2/ext2_common.cpp#L97))

**`get_or_alloc_block`——独立 `zbuf`,不再「写完重读」覆盖父 array**。这条路径上要给新分配的块清零并写盘。老代码用 `block_buf_`,清零那一下就把刚读进来的 indirect 数组覆盖了,所以老逻辑只能「写完再重读父数组」;现在给清零一块独立 `zbuf`,父 array 的 `buf` 原封不动:

```cpp
KmBuf zbuf(4096);
if (!zbuf || !zero_and_write_block(data_blk, zbuf.get())) { free_block(data_blk); return 0; }
// zbuf was a separate buffer, so child_ptrs (the child array in child_buf)
// is still intact -- patch and write.
child_ptrs[idx2] = data_blk;
if (!write_block(child_blk, child_buf.get())) { ... }
```

([ext2_inode.cpp:446-453](libs/ext2/ext2_inode.cpp#L446))。源码注释把这点写得很直白([ext2_inode.cpp:376-381](libs/ext2/ext2_inode.cpp#L376)):「the old shared-block_buf_ code had to re-read the parent each time because zeroing the child clobbered the only buffer」。迁完之后 double-indirect 的两层 walk 各自一块 `KmBuf`(`di_buf` / `child_buf`),互不踩。

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

([ext2_inode.cpp:48-63](libs/ext2/ext2_inode.cpp#L48))`write_disk_inode` 同款([ext2_inode.cpp:65-87](libs/ext2/ext2_inode.cpp#L65))。

同时,`locate_inode_block` 从「定位 + `read_block` 它」改成**只做算术定位**(算 `target_block` 和 `within_block_offset`),把「读块」交给上层各自的 `KmBuf`:

```cpp
// Pure arithmetic bounds check; the block read is the caller's job
// (read_disk_inode / write_disk_inode each use their own KmBuf so two
// CPUs touching different inodes don't share block_buf_).
```

([ext2_inode.cpp:38-40](libs/ext2/ext2_inode.cpp#L38))。这一改直接去掉一处共享读写——定位逻辑本来就不需要真读块,读是上层的活,让它显式地用自己的 buffer 读。

**bitmap alloc/free** —— 每次进 `alloc_block` / `free_block` 自己 `KmBuf`:

```cpp
KmBuf blk_buf(4096);
if (!blk_buf || !read_block(bitmap_block, blk_buf.get())) { ... }
auto* bitmap = blk_buf.data();
```

([ext2_block.cpp:43-50](libs/ext2/ext2_block.cpp#L43),`free_block` 见 [ext2_block.cpp:124-128](libs/ext2/ext2_block.cpp#L124))。

**`Ext2FileOps::read`** —— 用 `scratch.data()`(主线三已贴)。**directory readdir** —— `Ext2DirOps::readdir` 在 `ext2_dirops.cpp` 里,每个目录块自己 `KmBuf buf(4096)`([ext2_dirops.cpp:73](libs/ext2/ext2_dirops.cpp#L73))。同理,目录写路径 `add_dir_entry`(在另一个文件 `ext2_directory.cpp`)也是每个目录块一块独立 `KmBuf dir_buf`([ext2_directory.cpp:43-50](libs/ext2/ext2_directory.cpp#L43)),逻辑一致。

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

([ext2.hpp:447-457](libs/ext2/ext2.hpp#L447))

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

([ext2_directory.cpp:402-420](libs/ext2/ext2_directory.cpp#L402))

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

([ext2_directory.cpp:434-453](libs/ext2/ext2_directory.cpp#L434))

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

([ext2.hpp:496](libs/ext2/ext2.hpp#L496))。`alloc_block` / `free_block` 进去先拿这把锁:

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

([ext2_block.cpp:21-29](libs/ext2/ext2_block.cpp#L21))`free_block` 同款([ext2_block.cpp:104-106](libs/ext2/ext2_block.cpp#L104))。

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

([inode.hpp:146-151](kernel/fs/inode.hpp#L146))。默认是 `NotImplemented`([inode.cpp:94](kernel/fs/inode.cpp#L94)),ext2 override 它来实现 `O_TRUNC` / `ftruncate` 的截断语义:

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

([ext2_common.cpp:343-365](libs/ext2/ext2_common.cpp#L343))

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

([page_cache.hpp:127](kernel/mm/page_cache.hpp#L127) 声明,[page_cache.cpp:220-249](kernel/mm/page_cache.cpp#L220) 实现)

这两个为什么是 ext2 的依赖而非独立 feature?因为新 ext2 的行为(`O_TRUNC` 走 `truncate`、write 直写盘)需要它们做前提,不补 ext2 就跑不对。补到 parity 是搬家的连带账,不是另外的功能扩展。

## 范围与边界(诚实说)

这一章的 race 治理有几条没收尾的口子,摊开讲清,别让读者读完以为「ext2 已 SMP-safe」。

- **没搭 host 上的确定性竞态回归(TSAN)**。能在 host 上脱开 QEMU 直接跑 ext2 的真逻辑、再上 TSAN——也就是给 ext2 配一层 host mock I/O、外加一个能并发压它的测试——是这类 `block_buf_` race 的**确定性回归工具**:TSAN 能秒级报「线程 A 读、线程 B 写同一块内存」,这正是 QEMU forensics 漏掉的那一类 race。可这次没做。原因是 host test 基建有预存债:跑 `cmake --build build --target test_ext2_ops` 会直接报 `fatal error: fs/ext2/ext2_types.hpp: No such file or directory`——这个测试的 `#include "fs/ext2/ext2_types.hpp"` 还指向搬家前的旧路径(`kernel/fs/ext2/` → `libs/ext2/` 这次搬家把 include 打断了),mock 层也因此连不上。在破损基座上盖一层 host mock I/O 风险大,所以这次 race 修复**只靠 `run-kernel-test` 跑全绿验证,没有 host TSAN 的确定性回归**。直说:`block_buf_` race 在这里是「逻辑上根治(每个 SMP 路径都换了 per-call `KmBuf`、grep 无残留)、回归上未确定性验证」。这是真实的测试缺口,不能因为 `run-kernel-test` 绿就当成了事——等 host test 债清完(include 路径修顺)再补这一层。
- **`unlink` 的跨并发快照缓冲还没治**。主线五末尾已经诚实标注:`unlink_ptr_buf_` / `unlink_child_buf_` 治的是「单次 unlink 内部 free 过程的 clobber」,不治「两个 CPU 并发 unlink 同一 ext2 实例」那层共享快照 race——那层需要一把 per-instance `unlink_lock_`,留 follow-up。这章不假装它已 SMP-safe。
- **truncate 是 shrink-only,孤儿块不回收**。主线七已说,`O_TRUNC` 截断掉的孤儿数据块不释放,是已知 leak(hobby-os 式)。read 不超过 `i_size` 所以非正确性问题,只浪费磁盘;完整的孤儿块回收留 follow-up。
- **只讲「搬独立库 + 治 `block_buf_` 成 SMP-safe」这一根绳**。host 上的 mock I/O / TSAN 测试基座不教怎么搭(等 host test 债清完是独立章);`ext2_dirops.cpp` 这种「搬家顺带的结构整理」也只一句话带过,不教拆分方法论。
- **ext4 extent 读路径不展开**:`ext2_extent.cpp` 已经接进来了(`resolve_disk_block_` 里 `inode_has_extent_tree(disk)` 走 extent 公共解析器,depth-0 leaf 直接给块号),本章点到「它接进来了」即可。extent tree 的语义、树遍历是后续章的内容,不是这根 race 绳的一部分。
- **目录读写路径(`lookup_in_dir` / symlink readlink 等)的 per-call `KmBuf`**已随这次搬家一起到位,本章按合并态讲,不展开每条目录路径的迁移细节。

> 这一章的 race 修复,`run-kernel-test` 跑全绿是验证证据(不是战绩)。它站得住的真正理由,是每条 SMP 路径都换了 per-call `KmBuf`、`grep` 确认 `block_buf_` 不再出现在并发路径上、配套的 `block_alloc_lock_` 串行了 bitmap RMW、unlink 的 indirect 快照隔离了 free 过程的 clobber——这一套逻辑是闭环的。host 上 TSAN 的确定性回归是缺的最后一公里,债认了,等基座补齐再收。
