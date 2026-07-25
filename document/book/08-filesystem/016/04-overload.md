---
title: 04 · 主线三:治法 2——双参 SMP-safe 重载
---

# 主线三:治法 2——双参 SMP-safe 重载

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
