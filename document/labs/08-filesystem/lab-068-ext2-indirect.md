---
title: Lab 068 · ext2 间接块验证
---

# Lab 068 · ext2 间接块验证

> 对应 `document/book/08-filesystem/068-ext2-indirect.md`。验证档 **A 档**:这一章兑现 065 的承诺——把 ext2 的 double-indirect(`i_block[13]`)真做了,撤掉 065 的 4096 块 workaround。punchline 是 822 KB 文件(远超 single-indirect 268 KB 上限)真能在 1024 块的 ext2 上完整读写。验证靠 host 单测(算法门)+ dyn smoke(真内核门)双覆盖——run-kernel-test 自己守不住这层(测试文件都太小,走 direct)。

## 目标

确认六件事:

1. **ext2 三级块映射**:direct(`i_block[0-11]`)+ single-indirect(`[12]`)+ double-indirect(`[13]`);
2. **double-indirect 三层算术**:`offset/ptrs` + `offset%ptrs` 两层除余定位;
3. **写路径二次 read-modify-write**:scratch buffer 单块,写下层后改上层前必须重新读回(头号坑);
4. **write 放开了 file_block 上限**:原来只让写 direct,现在到 double-indirect;
5. **撤了 065 的 4096 块 workaround**:改回 1024(ext2 默认);
6. **double-indirect 双覆盖**:host 单测(算法 round-trip)+ dyn smoke(真内核 822 KB ldso 走 `i_block[13]`)。

## 步骤

### 1. host 单测:三层算法门

```bash
./build/test/test_ext2_ops
```

应看到 `30 passed, 0 failed`。里头关键的几条:single-indirect round-trip、**double-indirect round-trip**(在一个 512 块的 sim 盘上写 276 块 = 268 直/单 + 8 落 double,读回逐层 spot-read direct/single/double,断言 `i_block[12]`/`i_block[13]` 头指针置了)。这一层是「算法门」——host sim 镜像 kernel 的三层算术(`host_resolve_data_block` 共用 resolver),没有 kernel 的 scratch-dance,但三层算术和盘上布局跟 kernel 一致,所以能守算法不变量。

### 2. ext2 三级块映射 + 三层算术

```bash
sed -n '371,390p' kernel/fs/ext2_inode.cpp
```

应看到 double-indirect 的三层算术注释 + 代码:`ptrs_per_block = block_size/4`(1024 块下 256)、`di_base = DIRECT + ptrs`(268)、`offset = file_block - di_base`、`idx1 = offset/ptrs`(double 块里哪个 single 指针)、`idx2 = offset%ptrs`(那个 single 块里哪个数据指针)。这就是「文件块号落进 double-indirect 区后怎么定位到数据块」。

### 3. 写路径头号坑:二次 read-modify-write

```bash
sed -n '428,440p' kernel/fs/ext2_inode.cpp
```

应看到注释 `Re-read the double-indirect block: writing the child clobbered buf.` + 重新 `read_block(di_blk, block_buf_)`。这是头号坑:scratch buffer `block_buf_` 单块,每写一块覆盖它;分配下层块、写盘后,要改上层指针前**必须重新读回上层块**,不然改的是被覆盖的垃圾——毁 inode 元数据。三层逐层下钻、逐层落盘,每层回头改上层前都重新读,跟 single-indirect 既有写法对齐。

### 4. 撤 065 的 4096 块 workaround

```bash
sed -n '52,60p' scripts/create_ext2_disk.sh
```

应看到 `BLOCK_SIZE=1024` + 注释从「workaround:double-indirect 截断」改成「double-indirect 已支持,1024 块是 ext2 默认」。065 为了绕过 double-indirect 缺失改成 4096(把 single-indirect 上限从 268 KB 推到 4 MB);这一章真修完,改回 1024,让 double-indirect 真有文件走。

### 5. write 放开了 file_block 上限

```bash
grep -nE "file_block|max_file_block|EXT2_DIRECT_BLOCKS" kernel/fs/ext2_common.cpp | head
```

应看到 write 路径的 `file_block` 门从原来的 `> EXT2_DIRECT_BLOCKS`(只让写 direct 区,0..12)放开到 double-indirect 上限(`max_file_block`)。原来的门太紧——single-indirect 那 256 个槽基本没用上,文件写到 13 块就被 break。放开后写才能真正长进 single/double 区。

### 6. dyn smoke:真内核走 double-indirect(需 musl 工具链)

这是「真内核门」,要构建 musl 动态工具链(同 065):

```bash
tools/musl/build-musl.sh
tools/musl/build-hello-dyn.sh
cmake -B build -DCINUX_MUSL_DYN_SMOKE=ON
cmake --build build --target run-kernel-test 2>&1 | grep -iE "Hello from musl|hello-dyn|segment read failed" | head
```

应看到几行 `Hello from musl on CinuxOS!` + `hello-dyn 5/5 iters PASS`,**没有** `[ELF] segment read failed at offset 274432`(那是 double-indirect 坏时的现象)。822 KB 的 ldso 装在 1024 块的 ext2 上 = 803 块,远超 single-indirect 268 上限,execve 读它的 PT_LOAD 段 offset 274432 起**全部走新写的 `i_block[13]`**。这是 double-indirect 在真 QEMU 内核被走到且工作正常的铁证。

没编 musl 工具链的话,这道门跳过,靠步骤 1 的 host 算法门 + 步骤 2/3/4/5 的源码锚点验。

两腿汇总(run-kernel-test 自己,不依赖 musl):

```bash
cmake --build build --target run-kernel-test-all 2>&1 | grep -E "Tests: 9[0-9][0-9] passed" | tail -1
```

应看到单核 + `-smp 2` 两腿各 `997 passed, 0 failed`(ext2 内部修复,测试数不变)。

## 验收清单

- [ ] `./build/test/test_ext2_ops` 报 30 passed(含 double-indirect round-trip 算法门)。
- [ ] `ext2_inode.cpp:371` 三层算术(`offset/ptrs` + `offset%ptrs`),`ptrs_per_block=block_size/4`。
- [ ] `ext2_inode.cpp:431` 写路径重新 `read_block`(二次 read-modify-write,scratch buffer 单块的坑)。
- [ ] `ext2_common.cpp` write 的 `file_block` 门放开到 double-indirect 上限(原只 direct)。
- [ ] `create_ext2_disk.sh:60` `BLOCK_SIZE=1024`(撤 065 的 4096 workaround)。
- [ ] (编了 musl)`CINUX_MUSL_DYN_SMOKE=ON` 跑出 822 KB ldso 走 `i_block[13]`、5× Hello、无 `segment read failed`;两腿 997/0。

## 别做这些

- **别**指望 run-kernel-test 守得住这一层——测试文件(shell 17 KB、motd)都太小走 direct,碰不到 indirect;CI 也不跑 musl dyn smoke。这一层靠 host 单测(算法)+ dyn smoke(真内核)双覆盖,run-kernel-test 只证零回归。
- **别**在写路径省掉「重新读回上层块」——scratch buffer 单块,写下层后 buffer 已被覆盖,直接改上层指针就是改垃圾,毁 inode 元数据。必须二次 read-modify-write,跟 single-indirect 既有写法对齐。
- **别**以为 triple-indirect(`i_block[14]`)也做了——那是 `256³` > 16 GB 文件的事,显式不做。hobby OS 的盘才 8 MB,连 single-indirect 都用不满(除了那个 ldso)。
- **别**留 065 的 4096 块 workaround——真修完就该改回 1024(ext2 默认)。4096 只是绕过,不是正解;留着会让 double-indirect 一直没人走(822 KB 落 single-indirect 4 MB 上限内),真修就白做了。
- **别**以为 host 单测能验 scratch-dance——host sim 直访 `data_blocks[]`,没有 kernel 的单缓冲 dance。它守的是三层**算术**,scratch-dance 的正确性靠 dyn smoke(真内核)那道门。
