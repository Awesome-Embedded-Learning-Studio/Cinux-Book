---
title: 04 · 调试现场与验证
---

# 调试现场与验证

005 本身没有 notes(pack 里那两条 GP fault / MMIO 碰撞是 006/009 的,属后续 tag,不进本章)。但 ext2 这套有几个高发坑,值得当调试现场。

一是 **单 DMA 缓冲被覆盖**。整个 ext2 共用一个一页缓冲。读单间接块时,缓冲里是指针表;紧接着读数据块,指针表就被冲掉了。如果你写代码时把「读间接块」和「读数据块」之间还插了别的 read,或者把指针表地址存下来想稍后用——指望着缓冲内容还在——就拿到了被覆盖后的垃圾。规矩:每次 read_block 后立刻把需要的信息取走(拷贝或解析),绝不跨下一次 read 持有缓冲指针。

二是 **inode 定位数学算错**。`(ino-1)` 的 1-based、`bg_inode_table` 是块号不是字节、`index*inode_size` 的除法取块/取余——任何一个搞反,读出来的 inode 是错的。症状是 mount 时根 inode 读出来 mode 全错、magic 验过了但根目录认不出,或 lookup 一钻目录就崩。这道数学必须和规范逐一对照,host 单测专门焊它。

三是 **`rec_len == 0` 导致死循环**。扫目录项靠 `pos += rec_len` 步进。损坏数据或偏移算错时遇到 `rec_len==0`,pos 不前进、原地踏步死循环。那道 `if (entry->rec_len == 0) break;` 不能省。同样,`pos + 头大小 > block_size` 的越界检查也得有,防止读到块尾半个目录项。

四是 **全局 `g_ext2_instance` 限制单挂载**。InodeOps 的函数签名里没有 `Ext2*`,回调里要读块只能靠一个全局 `g_ext2_instance` 找回实例。这意味着同时**只能挂一个 ext2**——挂第二个会覆盖这个全局,前一个的 InodeOps 回调就指错了实例。这是 003 那套「InodeOps 是自由函数」设计的代价(003 的吐槽里说了,这套后来会被重构成虚函数、实例随 inode 走,这个全局也就没必要了)。这一章老实接受「单 ext2 挂载」的限制。

五是 **AHCI port 选错**。ext2 盘在 port **1**(port 0 是启动/内核盘)。`Ext2 ext2(ahci, 1)` 写成 port 0,会去读启动盘、读到一堆非 ext2 数据,magic 校验直接失败(`Invalid magic`)。盘在哪个 port 是 `cmake/qemu.cmake` 里挂的,驱动得对应。

## 验证

ext2 的逻辑(块大小换算、inode→块组/索引数学、直接/间接块翻译、目录项 rec_len 步进)大半能在 host 上镜像测。[test_ext2.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_ext2.cpp) 把这些纯算术抄了一份测:`block_size` 从 `log_block_size` 算对、inode 号到 group/index 的除法取余、直接块 vs 单间接块的 file_block→disk_block 翻译、目录项按 rec_len 步进:

```bash
ctest --test-dir build -R ext2 --output-on-failure
```

真盘、真 AHCI、真 ext2 在 QEMU 里验。先造盘——[create_ext2_disk.sh](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/scripts/create_ext2_disk.sh) 用 `mkfs.ext2 -b 1024 -O none -N 128` 造个 4MB ext2,再用 `debugfs` 往里塞 `/etc/motd` 和 `/hello.txt`(用 debugfs 是为了不用 root/挂载权限)。QEMU 把它挂到 AHCI port 1。机内测 [test_ext2.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_ext2.cpp) 验 mount 成功、按路径 lookup 找到文件、read 读出内容:

```bash
cmake --build build --target run-kernel-test
```

或直接跑完整内核,验收点是启动时那段 `[EXT2]` 日志——`Superblock valid: magic=0xef53`、`block_size=`、`groups=`,以及 `[VFS] ext2 mounted at /`。能读到磁盘上 `/hello.txt` 的内容(`Hello from ext2!`),整条「AHCI 读块 → ext2 解析 → VFS 暴露」链路就通了。这章的难点和前几章一样:正确性靠现象间接验证,所以 host 算术测(焊死 inode/块翻译)+ 机内测(真盘跑一遍)缺一不可。

## 下一站
