---
title: Lab 081 · tmpfs 验证
---

# Lab 081 · tmpfs 验证

> 对应 `document/book/08-filesystem/081-tmpfs.md`。验证档 **A 档**:这一章交付的是 `/tmp` 真挂上、能在里头 `mkdir`/`touch`/`echo`/`cat`、运行时 `mount`/`umount` 真能挂真能卸。注意 tmpfs **没有 host 单测**——核心逻辑依赖 kernel-only 路径(`inode_ref`、`kernel/lib/string.hpp`),host 链不进来,所以机制测走 kernel harness;boot 真 `/tmp` 靠 `make run` 冒烟 + busybox smoke。

## 目标

确认五件事:

1. **tmpfs 是内存型虚拟 FS 范式的第三次复用**(DevFS 064 / ProcFS 067 立的模子),但内容是真数据(住堆上的 `uint8_t* data`);
2. **写路径三件事**:4KB 对齐摊销扩容、gap 零填防 stale 堆字节、`is_page_cacheable()` 默认 false 绕开磁盘 PageCache;
3. **目录树是单向兄弟链表**(`first_child`+`next_sibling`,无上限,头插),支持运行时 `create`/`mkdir`/`unlink`;
4. **两条挂载通路**:boot 静态 `g_tmpfs` unowned、`sys_mount` 堆对象 owned=true,差别全在 `vfs_mount_add` 那个 bool;
5. **boot 真挂 /tmp**:`make run` 见 `[TMPFS] mounted at /tmp`,busybox 能在里面读写建删。

## 步骤

### 1. kernel 测试:tmpfs 机制(无 host 单测)

tmpfs 核心逻辑走 kernel harness(不是 host 单测):

```bash
cmake --build build --target run-kernel-test 2>&1 | grep -iE "tmpfs" | head
```

应看到 `test_tmpfs::test_mount_and_root` 等十项 PASS(`test_tmpfs.cpp:287` 的 `TmpFs` section):mount+root、create/write/read 往返、append+offset read、truncate 缩放、**跨 4KB 边界扩容 + gap 零填**、mkdir+readdir+嵌套 lookup、stat(file+dir)、unlink(文件 + 空目录/非空目录拒删)、负面路径(缺项/重名/穿文件下走)。测试用**栈局部 `TmpFs`** 直驱 `InodeOps`(`TmpFs tfs; tfs.mount();`),不走挂载表/syscall,确定性高。

> 为什么没 host 单测?`tmpfs.cpp` 虽然没 `kprintf`(纯逻辑),但它依赖 `kernel/fs/file.hpp`(`inode_ref`)、`kernel/lib/string.hpp`(`memcpy`/`memset`/`strlen`)这些 kernel-only 路径,host 链进来缺符号。这跟 DevFS(有 `CharSink` 注入缝、host 可测)不一样,跟 ProcFS(直读 registry、host 不可测)更像。要 host 可测得抽注入缝,留 follow-up。

### 2. syscall 级真挂测

还有一例在 syscall 测试里,验证 tmpfs 经挂载表 + syscall 路径端到端通:

```bash
grep -n "setup_tmpfs_at_tmp\|test_sys_open_creates_tmpfs_file_with_o_creat" kernel/test/test_vfs_syscall.cpp
```

应看到 `setup_tmpfs_at_tmp`(`test_vfs_syscall.cpp:80`)把堆 `TmpFs` 真注册进 `/tmp` 全局挂载表(`vfs_mount_add("/tmp", tfs)`,默认 2-arg 即 `owned=false`),然后 `test_sys_open_creates_tmpfs_file_with_o_creat`(`test_vfs_syscall.cpp:224`)用 `do_openat_kernel(O_CREAT)` 在 `/tmp` 下建文件、lookup+stat 验内容真落在 tmpfs 里。这例补上了「机制测」缺的那一腿——经 `vfs_resolve` 命中挂载表、走真 syscall 路径。

### 3. 数据存哪:TmpNode + 写路径三件事

```bash
sed -n '19,29p' kernel/fs/tmpfs/tmpfs.cpp
```

应看到 `struct TmpNode`:内嵌 `Inode` + `fs/fs_private` 指回自身(从 DevFS 的「单例 FS 指自己」升级到「per-node 指自己」)、`parent`/`next_sibling`/`first_child` 三条链、`name[kTmpfsNameMax]`,最后三件套 `uint8_t* data` + `uint64_t capacity` + `uint64_t size`(DevFS 没这部分,它不存数据)。

再看写路径:

```bash
sed -n '126,163p' kernel/fs/tmpfs/tmpfs.cpp
```

`TmpFileOps::write` 三件事一眼能看到:**(1)** `need > capacity` 时 `round_up_capacity(need)`(`tmpfs.cpp:35-41`)按 `kTmpfsGrowthAlign = 4096`(`tmpfs.hpp:58`)向上取整、`new uint8_t[newcap]`、拷旧前缀、`delete[]` 旧;**(2)** `offset > size` 的 gap `memset` 零填(`tmpfs.cpp:146-148` 扩容腿、`tmpfs.cpp:152-156` 原地腿);**(3)** 最后 `memcpy(node->data + offset, buf, count)` 真写入。

> **`is_page_cacheable()` 的「故意不做」**。grep 一下 tmpfs 的**实现文件**里有没有覆写这个虚函数:

```bash
grep -n "is_page_cacheable" kernel/fs/tmpfs/tmpfs.cpp
```

应看到**零命中**——`TmpFileOps`/`TmpDirOps` 没有任何一行覆写 `is_page_cacheable()`。(注意别把 `.hpp` 一起 grep:`tmpfs.hpp:27` 的文件头注释里提到了这个函数名,会多出一条说明性注释的命中,跟「有没有覆写」是两码事;只搜 `.cpp` 最干净。)再看基类默认:

```bash
sed -n '99,101p' kernel/fs/inode.cpp
```

`InodeOps::is_page_cacheable()` 默认返 `false`(源码是裸 `return false;`)。于是 `sys_read.cpp:48` / `sys_write.cpp:53` 的 gate 都判否,直连 `ops->read` / `ops->write`,tmpfs 内容永远不进 `g_page_cache`。**正确性来自一个被故意留在默认值的虚函数,不是显式代码**——这是特性(内存型 FS 不该进磁盘 cache),不是漏。

### 4. 目录树:单向兄弟链表代替定长表

对照 DevFS 的定长表:

```bash
grep -n "DEVFS_MAX_NODES\|nodes_\[" kernel/fs/devfs/devfs.hpp
```

应看到 `DEVFS_MAX_NODES = 16`(`devfs.hpp:40`)+ `DevNode nodes_[DEVFS_MAX_NODES]`(`devfs.hpp:179`)——定长、`mount()` 时焊死。tmpfs 不一样:

```bash
sed -n '314,346p' kernel/fs/tmpfs/tmpfs.cpp
```

`make_node`(create/mkdir 共用):校验名字长 → `find_child` 去重(返 `AlreadyExists`)→ `new TmpNode{}` value-init(指针 nullptr、size 0)→ `fs_private` 指回自身 → ops 按类型从 FS 单例取(同类共享一个 ops)→ **头插进 `dir->first_child`**(`node->next_sibling = dir->first_child; dir->first_child = node;`)。无上限,跟 DevFS 的定长 16 形成对照。

再看 unlink 怎么摘链:

```bash
sed -n '256,288p' kernel/fs/tmpfs/tmpfs.cpp
```

`unlink` 带 `prev` 指针(单向链表删中间节点必须记前驱),`prev == nullptr` 改头、否则 `prev->next_sibling = c->next_sibling`。注意 `tmpfs.cpp:277-279`:非空目录拒删返 `Error::IOError`(因为 `Error` 枚举没 `DirectoryNotEmpty`),syscall 边界映射 `kEio`。**契约满足(op 失败),但 errno ≠ Linux 的 `ENOTEMPTY`**——这是已知缺口,代码注释明说。

### 5. 两条挂载通路:差别全在 owned bool

boot 通路:

```bash
sed -n '33,50p' kernel/fs/tmpfs/tmpfs_init.cpp
```

应看到静态局部 `TmpFs g_tmpfs;`(`tmpfs_init.cpp:33`,命 = 整个内核运行)→ `mount()` → `vfs_mount_add("/tmp", &g_tmpfs)`(**默认 2-arg,`owned=false`**)→ `kprintf("[TMPFS] mounted at /tmp")`。boot 用静态是因为 `/tmp` 必须活到关机,挂载表只是中途登记指针;`owned=false` 让 `umount2("/tmp")` 只摘槽不释放(否则 `delete` 静态对象 = UAF)。

运行时通路:

```bash
sed -n '51,67p' kernel/syscall/sys_mount.cpp
```

`do_mount_kernel` 看到 `fstype == "tmpfs"`:`unique_ptr<TmpFs>` 持 `new TmpFs()` → `mount()` → `release()` 交裸指针 → `vfs_mount_add(target, fs, /*owned=*/true)`(`sys_mount.cpp:61`)。**`owned=true` 是关键**。错误腿(mount 失败 / 表满)都有显式 `delete` 回收。

看挂载表怎么按 owned 决定删不删:

```bash
sed -n '86,100p' kernel/fs/vfs_mount.cpp
```

`vfs_mount_remove` 看到 `owned=true` 就 `delete g_mount_table[i].fs`(`vfs_mount.cpp:93-95`),`TmpFs` 析构(`tmpfs.cpp:369-373`)递归 `free_tree`(`tmpfs.cpp:351-363`)释放整棵树;`owned=false` 只摘槽(`vfs_mount.cpp:96-98`)。**实际决定生命周期的是「挂载表登不登记所有权」,不是「对象在哪创建」**。

> 顺手核一下 boot 顺序:

```bash
grep -n "devfs::init\|procfs::init\|tmpfs::init" kernel/proc/init.cpp
```

应看到 `devfs::init`(`init.cpp:137`)→ `procfs::init`(`init.cpp:141`)→ `tmpfs::init`(`init.cpp:145`),tmpfs 在最后挂。

### 6. boot 真挂 /tmp + busybox smoke

冒烟(test kernel 不走 boot,要起真内核):

```bash
cmake --build build --target run
```

boot 序列应出现 `[TMPFS] mounted at /tmp`(`tmpfs_init.cpp:48` 的 kprintf)。进 shell 后用 busybox 做四组操作:

```bash
# (1) 可写可读
echo hello-tmpfs > /tmp/hello.txt
cat /tmp/hello.txt          # 应见 hello-tmpfs

# (2) 目录可建可列可删
mkdir /tmp/build
touch /tmp/build/a.o /tmp/build/b.o
ls /tmp/build               # 应见 a.o b.o
rm /tmp/build/a.o
ls /tmp/build               # 应只剩 b.o

# (3) 容量按 4KB 扩容——写超过 4096 字节
head -c 8192 /dev/zero > /tmp/big.bin    # /dev/zero 见 064 DevFS
stat /tmp/big.bin            # st_size 应是 8192(具体 stat 输出看 busybox 版本)

# (4) 运行时 sys_mount 真挂真卸
mount -t tmpfs none /mnt/tmp
touch /mnt/tmp/gone
ls /mnt/tmp                  # 应见 gone
umount /mnt/tmp
mount -t tmpfs none /mnt/tmp
ls /mnt/tmp                  # 应空——owned backend 真 delete,不是 stale 残留
```

第 (4) 组是关键验证:`umount` 后再 `mount` 同路径,文件不见了——证明 `sys_mount` 路径的 `owned=true` 让 `vfs_mount_remove` 真的 `delete` 了那个堆 `TmpFs`(`free_tree` 递归回收),新 `mount` 是个全新实例,不是上个实例的 stale 残留泄漏进来。

> 第 (3) 组验证容量扩容,但 `dd`/`head` 写 `/dev/zero` 不直接验 gap 零填(全零写进去看不出 gap)。要亲手摸 gap 零填,设计一个 offset 续写的小场景:先写 50 字节非零内容,再用 `dd seek=` 或 busybox 的 offset 写法在 4090 处续写,再读 [50,100) 那段——应是零,不是上个堆用过的字节。kernel 测试 `test_grow_past_4k_boundary_and_gap`(`test_tmpfs.cpp:138`)就是干这个的,host 侧 busybox 复现留给读者自己折腾(诚实标注:busybox 的 offset 写入工具有限,这步在 QEMU 里不一定好做,核心证据靠 kernel 测试那例就够)。

两腿汇总:

```bash
cmake --build build --target run-kernel-test-all 2>&1 | grep -E "Tests: [0-9]+ passed" | tail -2
```

应看到单核 + `-smp 2` 两腿各 `N passed, 0 failed`。**N 须在 Book 工作树真跑后填**——基线 `CINUX_TEST_BASELINE` 默认 875(`scripts/check_test_count.sh:14`),tmpfs 的 10 例机制测 + 1 例 syscall 真挂测都进 `big_kernel_test`。源仓库 dev note 记的数是源仓库的,Book 侧须独立验证,不能照抄。

## 进阶题(不给答案,留给真做过的读者)

这两题故意不给标准答案——题里点出的缺口,章节「这章没做的」已经交代过原因。lab 这儿只给场景和观察方法,答案你自己去跑出来、对照章节理解为什么会这样。别在 shell 里折腾(shell 的 `rm`/`open` 不在同一个进程,行为绕),写个小 userland 程序试最干净。

**题一:last-close 语义缺口**。写个小 userland 程序:`open("/tmp/foo", O_CREAT|O_WRONLY)` 写点东西进去,**不 close**,直接 `unlink("/tmp/foo")`,然后从那个还开着的 fd `read`(或者 `write` 再 `read`)。观察发生了什么——返回值、是否崩、读到的内容。对照章节「这章没做的」里「无 inode 级 last-close 语义」那一条,理解为什么 Linux 的 unlink-with-open-fd 需要 inode refcount,以及为什么本章把它留作 follow-up。你的观察结论写不写出来都行,关键是亲手踩到这个简化。

**题二:非空目录 errno 缺口**。写个小 userland 调 `mkdir("/tmp/d")` → `touch /tmp/d/x` → `rmdir("/tmp/d")`(或 `unlink` 目录),然后 `perror` 看 errno。再在 Linux 上跑同样程序对比 errno 值。观察两边 errno 差在哪。对照章节「非空目录删除返 EIO 而非 ENOTEMPTY」那一条,理解「契约满足(op 确实失败了)但 ABI 不精确」是什么滋味——以及为什么子模块的 `Error` 枚举缺项会让一个语义清晰的失败落成语义模糊的 errno。

## 验收清单

- [ ] `run-kernel-test` 见 `test_tmpfs` 十项 PASS(无 host 单测——核心逻辑依赖 kernel-only 路径)。
- [ ] `test_vfs_syscall.cpp:224` `test_sys_open_creates_tmpfs_file_with_o_creat` PASS(syscall 级真挂测,经挂载表 + `do_openat_kernel(O_CREAT)`)。
- [ ] `tmpfs.cpp:19-29` `TmpNode` 内嵌 `Inode` + `fs_private` 指回自身 + `data/capacity/size` 三件套;`tmpfs.cpp:126-163` write 三件事(4KB 扩容 + gap 零填 + memcpy)。
- [ ] `grep is_page_cacheable kernel/fs/tmpfs/tmpfs.cpp`(只搜 .cpp)零命中;`inode.cpp:99-101` 默认返 false;`sys_read.cpp:48`/`sys_write.cpp:53` gate 判否直连 ops。
- [ ] `tmpfs.cpp:314-346` `make_node` 头插单向兄弟链表;`tmpfs.cpp:256-288` unlink 带 prev 摘链,非空目录返 `Error::IOError`(`tmpfs.cpp:277-279`)。
- [ ] `tmpfs_init.cpp:33-50` boot 静态 `g_tmpfs` + `vfs_mount_add("/tmp", &g_tmpfs)` 默认 owned=false;`sys_mount.cpp:51-67` 堆对象 `owned=true`;`vfs_mount.cpp:93-95` owned-aware delete。
- [ ] `make run` 见 `[TMPFS] mounted at /tmp`;busybox smoke 四组(读写/建删/扩容/mount-umount-remount)全过;`run-kernel-test-all` 两腿 passed ≥ 基线 875。

## 别做这些

- **别**找 host 单测——tmpfs 核心逻辑依赖 kernel-only 路径(`inode_ref`/`kernel/lib/string.hpp`),host 链不进来。机制测走 kernel harness。要 host 可测得抽注入缝(同 DevFS 的 `CharSink` 思路),留 follow-up。
- **别**把 `is_page_cacheable` 的 grep 范围扩到 `.hpp`——`tmpfs.hpp:27` 文件头注释提到了这个函数名,会多出一条命中干扰判断。只搜 `tmpfs.cpp`,零命中才说明没覆写。
- **别**以为 tmpfs 内容进 PageCache——它**不进**。`is_page_cacheable()` 默认 false 是特性(内存型 FS 不该进磁盘 cache),不是缺陷。tmpfs 没有任何一行覆写这个虚函数,靠基类默认逃生。
- **别**照抄源仓库 dev note 的测试数(1070/1081 那类)——那是源仓库的,Book 侧须用 `scripts/check_test_count.sh` 真跑后独立验证(基线 875,真跑后填实际数)。
- **别**断言非空目录删除的 errno 等于 `ENOTEMPTY`——`Error` 枚举没这枚项,tmpfs 返 `Error::IOError` → `kEio`。测试只能断言「op 失败」(返回非零),不能断言具体 errno 值。
- **别**以为 `ln -s`/`mv`/`link` 在 `/tmp` 下能用——tmpfs 的 `TmpFileOps`/`TmpDirOps` 没覆写 `symlink`/`link`/`rename`,落到基类返 `NotImplemented` → `kEnosys`。busybox `ln -s`、`mv` 在 `/tmp` 下会失败,这是已知缺口(见章节「这章没做的」)。
- **别**以为「先 open 再 unlink」安全——tmpfs 无 last-close 语义,`unlink` 直接 `delete node`,open fd 立刻悬垂。依赖 close-before-unlink(GCC 临时文件模式正是如此),真 last-close 留 follow-up。
- **别**误标 boot 挂载 `owned=true`——会让 `sys_umount2("/tmp")` 在 `vfs_mount_remove` 里 `delete` 静态 `g_tmpfs`,双重释放/崩溃。boot/static 接线永远 `owned=false`(默认 2-arg),只有 `sys_mount` 这种「对象是调用方 new 出来的」路径才传 `true`。
- **别**指望 `/proc/mounts` 列出当前挂载——ProcFS 动态节点扩展没做,busybox `mount`(无参)列不出。但挂载本身是生效的(`vfs_resolve` 能命中),用 `ls /mnt/tmp` 之类间接验。
- **别**指望 `size=`/`mode=` 挂载选项或 swap 回收——都没做。tmpfs 理论上可吃光全部堆,无配额。MS_*/MNT_* flags 全接受但忽略。

---

##
