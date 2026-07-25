---
title: 04 · 调试现场与验证
---

# 调试现场与验证

004 没有 notes,但系统调用接 VFS 这一路有几个高频坑。

一是 **规范地址检查漏掉或写错**。`sys_open`/`sys_read`/`sys_write`/`sys_getdents` 开头那几行 bit47 判定不能省。漏了,用户传个内核态地址(高位全 1),内核就 `reinterpret_cast` 去读那块内核内存——要么读到一堆内核数据泄给用户,要么触发缺页。写错(比如条件反了)会把合法的用户指针也拒掉,所有 open/read 都返回 -1。这道闸是「系统调用收用户指针」的标配。

二是 **fd 0 的 stdin 特殊路径忘了保留**。如果把 sys_read 写成「一律走 VFS」,fd 0 在 FDTable 里没有 File(003 从 fd 3 起分配),`get(0)` 返回 nullptr,read 直接 -1——于是 stdin 读不到任何键。stdin/stdout 是「假文件」,不在 VFS 里,fd 0/1/2 的读写得保留键盘/屏幕那条老路,只有 fd≥3 才走 VFS。

三是 **read 完没推进 offset**。漏了 `file->offset += result`,`cat` 会死循环打印文件开头那几字节,永远读不到结尾——因为每次 read 都从 offset 0 开始,readdir/offset 推进是 VFS 层的职责,后端 read 是无状态的。看到「文件内容只打印了第一块、反复刷」,先查 offset 推进。

四是 **getdents 的 offset 语义搞错**。对目录,`file->offset` 是条目**下标**不是字节偏移。如果忘了 `file->offset++`,`ls` 会反复 getdents 到同一条(永远是 `.`),死循环。如果误把 offset 当字节偏移传给 readdir,readdir 的 index 参数就乱了,列出来的名字错位。

五是 **write 到只读文件系统没处理 -1**。ramdisk 的 `ramdisk_write` 恒返回 -1(003 讲过)。如果将来有个 `echo > file` 之类的命令不检查 sys_write 返回值、以为写成功了,其实啥也没写进去。只读文件系统上的写操作就是失败,调用方必须看返回值。

## 验证

系统调用接 VFS 的行为,主要在 QEMU 里验(涉及真陷入、真 VFS)。机内测 [test_vfs_syscall.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_vfs_syscall.cpp) 是这一篇最厚的一块(近 700 行):从用户视角(或直接调 sys_open 等)测 open 找到文件、read 读出内容、close 释放、getdents 列目录、各种错误路径(文件不存在、fd 非法、表满)。FDTable 的分配回收逻辑在 host 上镜像测 [test_fd_table.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_fd_table.cpp)。

```bash
ctest --test-dir build -R 'fd_table' --output-on-failure      # host
cmake --build build --target run-kernel-test                  # QEMU 机内测
```

最直观的验收是跑起来进 shell 敲命令。`cat hello.txt` 应输出文件内容(`Hello from Cinux!`),`ls` 应列出 initrd 里的文件名。这两条命令能跑,说明「libc 包装 → syscall 陷入 → VFS 解析 → ramdisk 后端 → 数据回到用户态」整条链路通了。这一篇的难点和前几篇一样:正确性靠现象间接验证,所以机内系统调用测(焊死 open/read/close/getdents 的行为)+ 真 shell 跑一遍缺一不可。

## 下一站
