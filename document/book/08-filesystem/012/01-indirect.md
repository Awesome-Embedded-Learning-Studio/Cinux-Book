---
title: 01 · ext2 间接块:double-indirect 真做
---

# ext2 间接块:double-indirect 真做

> 还记得 065(ELF 动态链接)那个藏得很深的坑吗?加载 822 KB 的 musl ldso 时,ext2 读到 offset 274432 处失败——`274432 = 268 × 1024`,正好是 ext2(1024 字节块)下 direct(12 块)+ single-indirect(256 块)的总和的**下一块**,也就是 **double-indirect 的起点**。当时 CinuxOS 的 ext2 驱动只处理 direct + single-indirect,double-indirect 那个分支直接 `break` 截断;065 为了不动 ext2,把盘改成 4096 字节块(single-indirect 上限推到 4 MB)绕过去了,把真修留作 follow-up。这一章兑现那笔债:**把 double-indirect(`i_block[13]`)的读和写都真做了,然后把盘改回 1024 字节块(ext2 默认)**,让 double-indirect 真正有人走。
>
> punchline 是 822 KB 的文件(那个 ldso)真能在 1024 字节块的 ext2 上完整读回来——822 KB 远超 single-indirect 的 268 KB 上限,必定走 double-indirect。这一章真正要讲的是 ext2 的**三级块映射**(direct / single-indirect / double-indirect)怎么用一层套一层的指针索引大文件,以及写路径里「新块零填写盘别擦掉父层指针」的陷阱怎么躲。

## 这章咱们要点亮什么

1. **ext2 的三级块映射**:direct(`i_block[0-11]`,直接指数据块)、single-indirect(`i_block[12]`,指一个「装满数据块指针」的块)、double-indirect(`i_block[13]`,指一个「装满 single-indirect 指针」的块)。一级套一级,换更大的寻址范围。
2. **double-indirect 的三层算术**:文件块号落进 double-indirect 区间后,用 `offset / ptrs_per_block` 和 `offset % ptrs_per_block` 两层除余,定位到那个数据块。
3. **写路径的头号坑:别让零填盘覆盖父层指针**:旧实现整 个驱动只有一个共享 `block_buf_`,新块零填写盘会擦掉父层指针;搬到 `libs/ext2/` 后给每个新分配块各自独立 `KmBuf zbuf` 零填写盘,父层指针待在它自己的 `di_buf`/`child_buf` 里不被覆盖,免去二次 read-modify-write。
4. **撤 workaround**:真修做完,把盘从 4096 块改回 1024 块(ext2 默认),让 double-indirect 真正有文件走它。
5. **算法门 + 真内核门双覆盖**:host 单测守三层算术(镜像 kernel 算法的纯 sim),dyn smoke 在真 QEMU 内核里让 822 KB ldso 真走 `i_block[13]`。

## ext2 怎么用三级指针索引一个文件

先看清 ext2 给一个文件预留的索引结构。每个 inode 有 15 个 `i_block` 槽:

- `i_block[0..11]` —— **direct**:每个直接指向一个数据块。12 个槽 = 12 块。
- `i_block[12]` —— **single-indirect**:指向一个「间接块」,这个间接块里装的不是数据,而是 `ptrs_per_block` 个**指向数据块的指针**。1024 字节块下 `ptrs_per_block = 1024/4 = 256`,所以这一级管 256 块。
- `i_block[13]` —— **double-indirect**:指向一个「双重间接块」,里面装的是 `ptrs_per_block` 个**指向 single-indirect 块的指针**,每个 single-indirect 块又管 256 块。这一级管 `256 × 256 = 65536` 块。
- `i_block[14]` —— **triple-indirect**:`256³` 块(>16 GB),这一章不做,hobby OS 用不到。

加起来 direct + single + double = 12 + 256 + 65536 ≈ 65804 块,1024 字节块下约 64 MB——够大了。065 撞的就是:822 KB 的文件 = 803 块,超过了 direct(12)+ single(256)= 268 块,落进 double-indirect 区,而当时驱动那儿是 `break`。

## 三层算术:文件块号怎么落到数据块

读/写一个文件块时,先按它落进哪一级分流。落进 double-indirect 区(块号 ≥ 12 + 256)后,用两层除余定位(`ext2_inode.cpp:371` 起):

```cpp
// double-indirect 区间内的逻辑索引布局:
//   offset = file_block - (EXT2_DIRECT_BLOCKS + ptrs_per_block)
//   idx1   = offset / ptrs_per_block   -> double-indirect 块里哪个 single-indirect 指针
//   idx2   = offset % ptrs_per_block   -> 那个 single-indirect 块里哪个数据指针
const uint32_t ptrs_per_block = block_size_ / sizeof(uint32_t);   // 1024/4 = 256
const uint32_t di_base  = EXT2_DIRECT_BLOCKS + ptrs_per_block;    // 12 + 256 = 268
const uint32_t offset   = file_block - di_base;
const uint32_t idx1     = offset / ptrs_per_block;
const uint32_t idx2     = offset % ptrs_per_block;
```

然后三层下钻:`i_block[13]` → 第 idx1 个 single-indirect 块 → 第 idx2 个数据块。读路径(`resolve_disk_block_`,遇 hole 即 `disk_block == 0` 落公共零填)和写路径(`get_or_alloc_block`,遇 hole 走 lazy-alloc)各自实现这套三层算术,算术结构同构、只是命中 hole 时一个返回 0、一个分配新块。

## 写路径的头号坑:别让零填盘覆盖了父层指针

写比读麻烦,因为要**分配**还不存在的间接块。这里有个这一章最大的坑,值得细讲。

分配 double-indirect 数据块的过程要碰三层:double-indirect 块本身(`i_block[13]`)、某个 single-indirect 块、最终的数据块。每新分配一个下层块,都得先**把那块全零写盘**(ext2 要求新块落盘时是干净的零),然后回头把它的块号**记进上一层**的指针槽——这是元数据,记错位置就毁了 inode。

危险在哪?新分配的块在落盘前要全零填。如果这个「零填 buffer」就是装着父层指针数组的那个 buffer,零填的瞬间父层指针就被擦成 0 了——改的是被覆盖的垃圾。旧实现里整 个驱动只有一个共享 `block_buf_`,每写一块都覆盖它,所以旧写法是**二次 read-modify-write**:分配并写下层块后,**重新 `read_block()` 读回上层块**,在重新读到的 buffer 里改指针再写回。

搬到 `libs/ext2/` 后的写法绕开了这个 dance:给**每个新分配的块各自一个独立的 `KmBuf zbuf`** 去零填写盘,父层数组则待在它自己的 `di_buf` / `child_buf` 里——`zero_and_write_block` 零填的是调用方的 zbuf,不是父层 buffer,所以写完下层后**父层指针槽还完整**,直接在原 buffer 里 patch 槽位再 `write_block` 即可,不需要重新读。源码注释把这一点说得很白(`libs/ext2/ext2_inode.cpp:376-381`):

```cpp
// Two independent KmBufs (di_buf for the double-indirect array, child_buf
// for each single-indirect child array): zero_and_write_block zeroes its
// caller's buffer, so giving each freshly-allocated block its own zbuf
// leaves the parent array intact -- no re-read needed after a child write
// (the old shared-block_buf_ code had to re-read the parent each time
// because zeroing the child clobbered the only buffer).
```

落到三层下钻就是:Level 0 给新分配的 double-indirect 块一个 `zbuf` 零填写盘;Level 1 读 `di_buf`(`di_ptrs[idx1] == 0` 时给新 child 块一个 `zbuf` 零填写盘,然后 patch `di_ptrs[idx1]` 写回);Level 2 读 `child_buf`(`child_ptrs[idx2] == 0` 时给新数据块一个 `zbuf` 零填写盘,然后 patch `child_ptrs[idx2]` 写回)。三层各自独立 buffer,zbuf 零填的全是与父层无关的临时段(`libs/ext2/ext2_inode.cpp:404-454`)。

> 不管是「重新读上层」还是「给下层单独 zbuf」,核心都一样:**写盘用的零填 buffer 不能就是装着父层指针的那个 buffer**。错一步就毁 inode 元数据(指针写错位置),而且不一定立刻崩——可能下次读那个文件才出错,debug 起来特别费劲。

顺带修了 write 的一个旧门:原来 `if (file_block > EXT2_DIRECT_BLOCKS) break;` 只允许写 direct 区(0..12),single-indirect 那 256 个槽基本没用上。现在放开到 double-indirect 上限(`max_file_block`),写才能真正长进 single/double 区——不然光有读路径,写不进去也是白搭。

## 撤 workaround:改回 1024 块

真修做完,065 那个 4096 块的 workaround 就该撤了。`create_ext2_disk.sh` 的 `BLOCK_SIZE` 从 4096 改回 1024(ext2 默认),注释从「workaround:double-indirect 截断」改成「double-indirect 已支持」。现存盘上的文件(shell 17 KB、motd、hello.txt)远低于 268 KB 的 single-indirect 上限,纯走 direct,零行为变;只有 822 KB 的 ldso(本地 musl sysroot 装盘时才有)现在走 double-indirect——它就是 double-indirect 的 opportunistic 端到端验证。

## 验证:算法门 + 真内核门

这一章的验证有个特别的地方:**没有 in-kernel 测试锻炼 indirect**。测试用的 shell 才 17 KB、motd/hello.txt 都几 KB,全走 direct,碰不到 indirect;CI 也不跑 musl dyn smoke。所以光靠 run-kernel-test 守不住这一层。得两道门一起上。

**第一道:host 单测守三层算法。** `test/unit/test_ext2_ops.cpp` 镜像 kernel 的三层算术,抽了个 `host_resolve_data_block(disk, inode, file_block, alloc)` 共用 resolver(跟 kernel 的解析逻辑同构,只是直访 `data_blocks[]` 没有 scratch-dance),加了 single + double 的 write→read round-trip 用例:在一个 4 group = 512 块的 sim 盘上写 276 块(268 直/单 + 8 落 double),读回来逐层 spot-read direct/single/double,断言 `i_block[12]`/`i_block[13]` 头指针都置了(其中含那条 double-indirect round-trip)。host sim 没有 scratch-dance,但**三层算术和盘上布局跟 kernel 完全一致**——这正是它能守算法不变量的价值。

**第二道:dyn smoke 在真内核走 double-indirect。** 这是关键的一步——host 守的是算法,真内核里 scratch-dance 对不对得真跑。建 musl sysroot + `build-hello-dyn.sh`,把 822 KB 的 ldso 装进 1024 块的 ext2(803 块,远超 single-indirect 268 上限),开 `CINUX_MUSL_DYN_SMOKE` 跑 `execve("/hello-dyn")`:内核读 ldso 的 PT_LOAD 段,offset 274432 起**全部走新写的 `i_block[13]` double-indirect**。串口 5× `Hello from musl on CinuxOS!` + `hello-dyn 5/5 PASS`,而且**没有** `[ELF] segment read failed at offset 274432`(那是 double-indirect 坏时的现象)。这是 double-indirect 在 QEMU 真内核被走到且工作正常的铁证,不再只靠 host 单测。

加上 run-kernel-test-all 两腿零回归(改的是 ext2 内部,不影响别的),三道门齐了。

## 这章没做的

- **triple-indirect(`i_block[14]`)**:那是 `256³` 块 > 16 GB 文件的事,hobby OS 用不到,显式不做。
- **完整 Linux ext2 写语义**(journal、extent、reservation):都不在,这是最小可用的间接块支持。

## 小结

- ext2 用三级指针索引文件:direct(`i_block[0-11]`)、single-indirect(`i_block[12]`,管 `ptrs_per_block` 块)、double-indirect(`i_block[13]`,管 `ptrs_per_block²` 块)。065 撞的就是 double-indirect 缺失。
- double-indirect 三层算术:`offset = file_block - (direct + ptrs)`,`idx1 = offset/ptrs`(double 块里哪个 single 指针)、`idx2 = offset%ptrs`(那个 single 块里哪个数据指针)。读路径(`resolve_disk_block_`,hole 落零填)和写路径(`get_or_alloc_block`,hole 走 lazy-alloc)各自实现这套算术。
- 写路径头号坑:旧实现只有一个共享 `block_buf_`,新块零填写盘会擦掉父层指针(旧解法是二次 read-modify-write);搬到 `libs/ext2/` 后给每个新分配块各自独立 `KmBuf zbuf` 零填写盘,父层指针待在自己的 `di_buf`/`child_buf` 里不被覆盖,免去重读。顺带放开 write 的 file_block 上限(原来只让写 direct)。
- 撤 065 的 4096 块 workaround,改回 1024(ext2 默认),让 double-indirect 真有文件走(822 KB ldso)。
- 双覆盖验证:host 单测守三层算法(镜像 kernel resolver)+ dyn smoke 真内核让 822 KB ldso 走 `i_block[13]`;run-kernel-test 两腿零回归。triple-indirect 不做。
