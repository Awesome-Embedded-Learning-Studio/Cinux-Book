---
title: 10 · 收尾:验证与下一站
---

# 收尾:验证与下一站

## 验证

006 的写测试分两层:host 单元测试(不碰真硬件,在内存镜像上跑 ext2 逻辑)和 QEMU kernel 测试(真跑、真写 AHCI 盘)。

**host 单元测试**,在 build 目录跑:

```bash
cmake --build build
ctest --test-dir build -R ext2 --output-on-failure
ctest --test-dir build -R ahci_write --output-on-failure
ctest --test-dir build -R shell_write --output-on-failure
```

覆盖面:`test_ext2_allocator`(块/inode 分配器——分配、释放、位图置位、计数同步)、`test_ext2_ops`(`creat`/`mkdir`/`write`/`unlink` 端到端)、`test_ext2_inode_ops`(经 `InodeOps::write`/`read`)、`test_syscall_ext2`(经系统调用号的端到端)、`test_ahci_write`(AHCI 写盘)、`test_shell_write`(shell 的 `touch`/`mkdir`/`rm`/`rmdir`/`echo >`)。

**QEMU kernel 测试**:

```bash
cmake --build build --target run-kernel-test
```

这里有个 006 新增的、值得专门讲的工程细节:`run-kernel-test` 现在会**先强制重建 ext2 镜像**。看 `cmake/qemu.cmake`:

```cmake
add_custom_target(regenerate-ext2-image
    COMMAND ${CMAKE_COMMAND} -E remove -f ${EXT2_IMAGE}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/create_ext2_disk.sh ${EXT2_IMAGE}
    ...
)
add_custom_target(run-kernel-test
    ...
    DEPENDS test-image ${AHCI_TEST_IMAGE} regenerate-ext2-image
)
```

为什么?因为 006 能写盘了。005 那会儿 ext2 是只读的,每次跑测试,盘上的内容都是 `create_ext2_disk.sh` 预先做好的固定样子,跑完不变。006 不行——一次测试里 `touch` 出来的新文件、改过的位图和计数,会**真的写进 ext2.img**。如果不重建,下一次测试就在一个「被上次测试写脏过」的盘上跑,状态污染,结果不可复现。所以每跑一次 kernel 测试前,先 `rm` 掉旧镜像、用脚本重新生成一个干净的。这条依赖是 006 写能力的直接副产品。

**预期现象。** 在 QEMU 的 shell 里(或测试里)做一串操作,验证写链路:

```text
$ touch /hello.txt            # sys_creat → Ext2::create
$ echo hi > /hello.txt        # creat + open + write
$ mkdir /somedir              # sys_mkdir → Ext2::mkdir
$ (读回 /hello.txt)           # 应得到 "hi"
$ rm /hello.txt               # sys_unlink → Ext2::unlink
$ rmdir /somedir              # sys_rmdir(空目录检查通过)→ unlink
```

写链路通的标志是「写进去的能读回来,内容一致」。至于「跨重启持久」——因为 `write_block` 经 `ahci.write` 把数据真的写进了 raw 盘镜像,持久性是这条链路的自然结果;只是 CI 为了测试隔离,默认每次 `regenerate` 一个干净盘,所以「持久」要在不重建镜像的两次运行之间才能直接观察到。

**故意触发那个 GP fault 复现一下**(在修复前的版本上)也能验证调试现场的结论:`touch /hello2.txt` 会让未打补丁的内核在 `Ext2::create` 里 #GP,打上 `syscall.S` 的 `subq $8, %rsp` 后消失。

## 下一站

006 让 ext2 能建、能写、能删,文件系统的「写」这一半补齐了。但你会发现 shell 现在有个别扭的地方:所有命令都得用从根开始的绝对路径——`/hello.txt`、`/somedir`。没有「当前工作目录」的概念,也就没有 `cd`、`pwd`;想看一个文件的大小、类型这些 inode 信息,也没有 `stat`。这些是文件系统作为一个「给人用的」系统还缺的拼图。下一章(007)会补上工作目录和 `stat`,顺带把这一章里那个每个 syscall 各写一份的 `split_pathname` 抽成公共的路径解析模块。怎么抽、cwd 挂在进程的哪里,那是 007 的事——我们这一章的 ext2,已经能老老实实地往磁盘上写东西了。

---

**参考**

- ext2 磁盘布局规范(The Second Extended Filesystem):块位图/inode 位图的位语义、`i_block[15]` 的直接/单间接/双间接/三间接划分、目录项 `rec_len`+`name_len` 布局、`i_links_count` 与 `bg_used_dirs_count` 的维护规则。这些是本章分配器、目录项增删、`mkdir` 链接计数的依据(005 已引,本章沿用)。
- System V AMD64 ABI:在执行 `call` 指令时 RSP 必须是 16 字节对齐——这是「调试现场」里 GP fault 根因的依据。参考 x86-64 psABI:<https://gitlab.com/x86-psABIs/x86-64-ABI>。
- ATA/ATAPI Command Set(ACS):`WRITE DMA EXT`(命令码 0x35,48 位 LBA),AHCI `write` 走的就是它(见 `ahci.hpp`)。
- Linux man-pages:`creat(2)`(已存在则截断为 0)、`mkdir(2)`/`rmdir(2)`(目录须为空)、`unlink(2)`(链接数归零则释放)的语义,以及 syscall 号 83/84/85/87 的复用:<https://man7.org/linux/man-pages/>。
