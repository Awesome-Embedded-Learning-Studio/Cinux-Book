---
title: 05 · 收尾:验证、没做的、小结
---

# 收尾:验证、没做的、小结

## 验证

三层验证,合起来才够。

**第一层:host 无 tmpfs 单测——讲清为啥没有**。`tmpfs.cpp` 虽然是纯逻辑(无 `kprintf`),理论 host 可链,但实际 Book 工作树没给 tmpfs 配 host 单测。原因跟 ProcFS 类似:`tmpfs.cpp` 依赖 `kernel/fs/file.hpp`(`inode_ref`)、`kernel/lib/string.hpp` 这些 kernel-only 路径,host 链进来缺符号。所以 tmpfs 的核心机制测走 kernel harness,不靠 host。这跟 DevFS(有 CharSink 注入缝、host 可测)不一样,跟 ProcFS(直读 registry、host 不可测)更像。

**第二层:kernel 测试,十例机制测 + 一例 syscall 真挂测**。`test_tmpfs.cpp` 的 `TmpFs` section(`test_tmpfs.cpp:287`)十例:mount+root、create/write/read 往返、append+offset read、truncate 缩放、跨 4KB 边界扩容+gap 零填、mkdir+readdir+嵌套 lookup、stat(file+dir)、unlink(文件+空目录/非空目录拒删)、负面路径(缺项/重名/穿文件下走)。测试用**栈局部 `TmpFs`** 直驱 `InodeOps`(`TmpFs tfs; tfs.mount();`),不走挂载表/syscall,所以确定性高、不污染全局 fd 表/挂载表。另有一例 syscall 级真挂测在 `test_vfs_syscall.cpp`:`test_sys_open_creates_tmpfs_file_with_o_creat`(`test_vfs_syscall.cpp:224`)靠 `setup_tmpfs_at_tmp`(`test_vfs_syscall.cpp:80`)把堆 `TmpFs` 真注册进 `/tmp` 全局挂载表(`vfs_mount_add("/tmp", tfs)`,默认 `owned=false`),然后 `do_openat_kernel(O_CREAT)` 建文件、lookup+stat 验内容真在 tmpfs 里。这例证明 tmpfs 经挂载表 + syscall 路径端到端通。

**第三层:make run 冒烟 + busybox smoke**。`make run` 起 QEMU,看 boot 序列出现 `[TMPFS] mounted at /tmp`(`tmpfs_init.cpp:48` 的 kprintf)。进 shell 后用 busybox 做一组操作:`echo hello > /tmp/hello.txt && cat /tmp/hello.txt`(可写可读、字节对得上)、`mkdir /tmp/build && touch /tmp/build/a.o && ls /tmp/build`(目录可建可列)、`rm /tmp/build/a.o && ls /tmp/build`(可删)、`mount -t tmpfs none /mnt/tmp && touch /mnt/tmp/x && umount /mnt/tmp && mount -t tmpfs none /mnt/tmp && ls /mnt/tmp`(运行时挂载/卸载/再挂,文件不见 = owned backend 真 delete)。

> 测试数字怎么填。`run-kernel-test-all` 两腿(单核 + `-smp 2`)的通过数,得在 Book 工作树真跑后填,不照抄源仓库 dev note 的数(那是源仓库的,Book 侧须独立验证)。`scripts/check_test_count.sh` 就是干这个的——基线 `CINUX_TEST_BASELINE` 默认 875(`check_test_count.sh:14`),tmpfs 的 10 例机制测 + 1 例 syscall 真挂测都进 `big_kernel_test`,真跑后 passed 数应 ≥ 基线 + 这些例。用户态真能用 `/tmp` 靠 boot 冒烟 `[TMPFS] mounted at /tmp` + busybox smoke 两腿绿间接证明,不是 `big_kernel_test` 的直接断言——三层证据合力,任一单独都不够。

## 这章没做的

- **无 inode 级 last-close 语义**。`unlink` 摘链后直接 `delete[] data + delete node`,完全不查 inode 的 refcount。内核里其实有个 open-description 引用计数(`Inode.refcount`,`inode_ref`/`inode_unref` 在 `file.cpp`),open fd 持一份。Linux 真语义是「`unlink` 时仍有 open fd 则延迟到 last close 释放」;Cinux 的 tmpfs 不看它,所以**「先 open 再 unlink」会有 use-after-free 风险**。这是诚实简化,依赖 close-before-unlink(GCC 临时文件模式正是如此),真 last-close 语义留 follow-up。
- **非空目录删除返 EIO 而非 ENOTEMPTY**。`Error` 枚举(Cinux-Base 子模块)无 `DirectoryNotEmpty` 项,`unlink` 非空目录返 `Error::IOError` → syscall 边界 `kEio`。契约层满足,errno 不精确,留子模块加枚举项后修。
- **symlink / hardlink / rename 未实现**。`InodeOps` 基类有 `symlink`/`link`/`rename` 这三个虚函数(`inode.cpp:86-97`),默认返 `Error::NotImplemented`;tmpfs 的 `TmpFileOps`/`TmpDirOps` 一个都没覆写,所以落到基类就是 `NotImplemented` → syscall 边界 `kEnosys`。busybox `ln -s /tmp/a /tmp/b`、`mv /tmp/a /tmp/b` 在 `/tmp` 下都会失败。GCC 编译中间产物模式不太依赖这些(rename 原子换名用得少),所以这章先不做,留 follow-up——这是 tmpfs 相对 Linux 的一个明显行为缺口,跟「非空目录返 EIO」是同类已知简化。
- **单 per-FS Spinlock 粗粒度**。一个 `Spinlock`(`tmpfs.hpp:102`)串行化所有树变更 + 内容 I/O。`/tmp` 专用低竞争场景够用,/tmp 上 GCC 多进程高并发未做并发压测。刻意不给 per-node 锁——是为了避开父/子嵌套加锁的 AB-BA 死锁。per-node 锁 / RCU 留 follow-up。
- **无内存上限 / 无 swap 支撑**。tmpfs 理论上可吃光全部堆,本实现无 `size=`/`mode=` 挂载选项、无 `max_blocks` 配额。Linux tmpfs 有 `size=` 挂载选项,Cinux 的没有。`write` 里 `new uint8_t[newcap]` 失败会抛(无 nothrow),kernel 端 new 失败的语义本章不覆盖。swap 回收、oom-kill 全无。
- **MS_* / MNT_* flags 全接受但忽略**。`sys_mount` 的 flags(`MS_NOSUID`/`MS_NODEV`/`MS_NOEXEC`/`MS_RDONLY` 等)注释明写「accepted for Linux ABI parity but not yet modelled」(`sys_mount.cpp:46`);`umount2` 的 `MNT_FORCE`/`MNT_DETACH`/`MNT_EXPIRE` 也是 `[[maybe_unused]]`(`sys_umount2.cpp:22`)。只读挂载、noexec、强卸忙挂载都没建模——挂出来都是可读写可执行。
- **mmap on tmpfs、大文件 offset 溢出防护**未做。
- **/proc/mounts 不存在**。busybox `mount`(无参)列挂载点需要它,ProcFS 动态节点扩展没做,所以 `mount` 命令列不出当前挂载——但挂载本身是生效的(`vfs_resolve` 能命中)。
- **内容不进 PageCache 是设计如此,不是缺陷**——`is_page_cacheable()` 默认 false 是特性。

## 小结

- tmpfs 是内存型虚拟 FS 范式(DevFS 010 / ProcFS 011 已立)的第三次复用:`FileSystem` 子类 + `InodeOps` 子类 + boot `_init.cpp`。差别只在「内容是不是真数据」——tmpfs 的内容是用户态写进去的字节,住在堆上。
- **数据存哪**:`TmpNode` 内嵌 `Inode` + `fs_private` 指回自身,从 DevFS 的「单例 FS 指自己」升级到「per-node 指自己」;文件内容住 `data/capacity/size` 三件套。写路径三件事:4KB 对齐摊销扩容、gap 零填防 stale 堆字节、靠 `is_page_cacheable()` 默认 false 绕开磁盘 PageCache——**正确性来自一个被故意留在默认值的虚函数,不是显式代码**。
- **目录树怎么长**:单向兄弟链表代替 DevFS 的定长表(`first_child`+`next_sibling`,头插,无上限);`make_node` 共用 create/mkdir,`unlink` 带 prev 摘链,`lookup` 真正多段 walk(扁平的 DevFS/ProcFS 不需要)。
- **两条挂载通路**:boot 静态 `g_tmpfs` unowned(`/tmp` 命超表,`umount2` 只摘槽);`sys_mount` 堆对象 owned=true(`umount2` 走 `free_tree` 回收整棵树)。差别全在挂载表那个 `owned` bool——**实际决定生命周期的是「挂载表登不登记所有权」,不是「对象在哪创建」**。
- 诚实边界:无 last-close 语义(`unlink` 立即删,先 open 再 unlink 有 UAF 风险)、非空目录删返 EIO 非 ENOTEMPTY、`symlink`/`link`/`rename` 未实现(返 ENOSYS)、单 per-FS 锁粗粒度、无 size 上限/无 swap、flags 全接受但忽略、`/proc/mounts` 不存在。这些不假装做了,留给后续工程债。
