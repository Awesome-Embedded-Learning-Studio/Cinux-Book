---
title: Lab 028b · 让 ext2 真的写进去:从命令到磁盘布局
---

# Lab 028b · 让 ext2 真的写进去:从命令到磁盘布局

> 028b 给 ext2 加了「写」。这个 lab 不让你重写一遍 ext2 的写代码(那是主书的活),而是让你**亲眼看见每一次「建/写/删」在磁盘上到底改了什么**:位图里哪一位翻了、哪个 inode 出现了、目录项怎么插进去的、空闲计数怎么同步的。工具是 `debugfs`——e2fsprogs 里那个能直接读写 ext2 镜像的瑞士军刀,Cinux 自己的 `create_ext2_disk.sh` 就是靠它往盘里塞文件的。我们用它做对照:你在 `debugfs` 里做一步,再去主书里查「Cinux 内核的哪个方法做了同样的事」。做完你会对 ext2 的写路径有手感,而不是只记得几个函数名。

## 实验目标

- 用 `debugfs` 在一块干净的 ext2 镜像上,亲手完成**建文件、写内容、建目录、删除**,并在每一步观察位图、inode、目录项、空闲计数的变化。
- 把 `debugfs` 的每一步操作,对应到 Cinux ext2 驱动里的具体方法(`alloc_inode`/`alloc_block`/`write_disk_inode`/`add_dir_entry`/`Ext2FileOps::write`/`unlink`),说清楚内核里是哪个函数负责产生你看到的磁盘变化。
- 撞上并理解三个 028b 真实的坑:未重建镜像读到脏数据、分配后忘了同步空闲计数导致漂移、写的 13KB 截断。
- (加分)在跑起来的 Cinux shell 里端到端走一遍,确认内核驱动的结果和 `debugfs` 观察到的一致。

## 前置条件

- 028 的只读 ext2 能挂载、能读出 `/hello.txt`(说明超块/BGDT/inode/目录项的读路径都通)。
- host 装了 `e2fsprogs`(`mkfs.ext2`、`debugfs`)。验证:`command -v debugfs`。
- 读懂主书第 028b 章的「写回的统一姿势」和「两个分配器」两节,理解 read-modify-write 和位图计数同步。
- 准备一块干净镜像:

  ```bash
  ./scripts/create_ext2_disk.sh /tmp/lab.ext2
  ```

  这会生成一个 4MB、block_size=1024、128 个 inode(`-N 128`)、关闭所有可选特性(`-O none`)的 ext2 镜像,里面已有 `/etc/motd`、`/hello.txt`、`etc/`。

> 全程我们只对这份**拷贝** `/tmp/lab.ext2` 操作,别动 build 目录里 CI 用的那份——028b 的 `run-kernel-test` 每次跑前会 `regenerate-ext2-image` 重建它,你手动改了也会被冲掉。

## 任务分解

下面每个任务都遵循同一个套路:**先记录操作前的状态,再操作,再对比**。ext2 的字段大多是「相对变化」,记下操作前的数字、操作后再看,变化方向对就算通过。

进入 `debugfs` 交互(写模式,这样能改):

```bash
debugfs -w /tmp/lab.ext2
debugfs:
```

### 任务 1:建文件 —— 看 inode 与目录项

先看「操作前」。根目录是 inode 2:

```text
debugfs: stat <2>          # 根 inode:记下 links、size
debugfs: ls /              # 根目录里现在有: . .. etc hello.txt (及它们的 inode 号)
debugfs: show_super_stats  # 记下 Free inodes count、Free blocks count
```

> `show_super_stats`(可简写 `ss`)打印超块和块组描述符。我们要盯的是超块里的 `Free inodes count` 和 `Free blocks count`,以及第 0 组描述符里对应的本组小计。不同版本的 `debugfs` 输出字段顺序略有差异,以你机器上的为准。

现在用 `debugfs` 建一个文件(这一步,等价于 Cinux 里 `touch /lab1.txt` 经 `sys_creat` → `Ext2::create`):

```text
debugfs: write /etc/hostname lab1.txt    # 把一个 host 文件写进镜像当 lab1.txt
```

> 如果你不想依赖 `/etc/hostname`,先 `echo hello > /tmp/seed.txt`,再 `write /tmp/seed.txt lab1.txt`。

再看「操作后」,逐项对比:

- `ls /`:根目录多了一项 `lab1.txt`,带一个**新分配的 inode 号**(记下它,假设是 `N`)。
- `stat <N>`:这个新 inode 的 mode 应是普通文件(`S_IFREG`,权限 `0644`)、`links = 1`、`size` 等于你写入的字节数。
- `show_super_stats`:`Free inodes count` 比「操作前」少 1。
- (可选)`testi <N>`:`debugfs` 会告诉你这个 inode「已使用」——对应内核 `alloc_inode` 在 inode 位图里置的那一位。

**对应到内核**:`ls /` 里出现的新项,是 `Ext2::create` 调 `add_dir_entry` 在根目录数据块里插的;新 inode 的 `links=1`/`REG|0644` 是 `create` 里初始化 `Ext2Inode` 时写的;`Free inodes count -1` 是 `alloc_inode` 里 `--sb_.s_free_inodes_count` 配 `write_superblock()` 的结果。

### 任务 2:写内容 —— 看数据块分配

任务 1 的 `write` 其实已经把内容写进去了。我们换个角度,专门看**数据块**。重新建一个空文件(等价于先 `creat` 再单独 `write`):

```text
debugfs: stat <2>          # 操作前:Free blocks count
debugfs: kill_file <上一个 lab1.txt 的 inode>   # 先清掉,腾回干净态(可选)
```

> `kill_file <inode>` 清除一个 inode。用它把任务 1 的文件删掉,观察 `Free inodes`/`Free blocks` **恢复**(对应内核 `unlink` 在 link 归零时的 `free_inode`/`free_block`)——这是任务 4 的预演。

建一个内容稍多(跨块)的文件,观察数据块:

```text
debugfs: stat <新文件 inode>
```

在 `stat` 输出里看 `BLOCKS:` 那一行和 `Blockcount`。一个非空文件至少占一个数据块;`stat` 会列出它占用的块号。对比 `show_super_stats`:`Free blocks count` 应比空文件时少。

**对应到内核**:`BLOCKS` 里列出的数据块,是 `Ext2FileOps::write` 通过 `get_or_alloc_block` → `alloc_block` 分配的;`Blockcount` 是 `write` 末尾更新的 `i_blocks`(单位 512 字节扇区)。注意主书讲过:部分块写要**先 `read_block` 再覆盖再 `write_block`**,否则会把同块其它内容擦掉——`debugfs` 替你处理了这点,但内核里这步不能省。

### 任务 3:建目录 —— 看 `.`/`..` 与链接计数

```text
debugfs: stat <2>          # 操作前:根 inode 的 links(记下来,假设是 L)
debugfs: mkdir sub
debugfs: stat <2>          # 操作后:根 links 应该变成 L+1
debugfs: stat <sub 的 inode>
debugfs: ls sub            # 应该看到 . 和 .. 两个项
```

这里要核对三件 ext2 目录语义的事(主书「建文件与建目录」一节讲过):

- 新目录 `sub` 的 inode:`mode` 是目录(`S_IFDIR`,权限 `0755`)、`links = 2`。为什么是 2?一个来自它自己的 `.`(指向自己),一个来自父目录里刚加的 `sub` 这一项。
- `sub` 的数据块里第一项是 `.`(inode 指向 `sub` 自己),第二项是 `..`(inode 指向根 inode 2)。`debugfs ls sub` 能看到这两个名字。这就是内核 `Ext2::mkdir` 里手写的那两条目录项。
- 根 inode 2 的 `links` 从 `L` 变成 `L+1`:因为新目录 `sub` 的 `..` 又指向了根,根多了一个「被引用」。这是 `mkdir` 里 `dir_disk.i_links_count++` 干的。
- `show_super_stats` 里第 0 组的「used directories」计数(`bg_used_dirs_count`)应 +1。

**对应到内核**:`.`/`..` 两条目录项是 `Ext2::mkdir` 写进新分配数据块的;根 `links+1` 是 `dir_disk.i_links_count++`;组目录计数 +1 是 `bgdt_[new_group].bg_used_dirs_count++` 配 `write_bgdt(new_group)`。这三个少改一个,`fsck` 就会抱怨。

### 任务 4:删除 —— 看资源回收

删掉任务 2 建的那个文件:

```text
debugfs: stat <父目录 inode>     # 操作前
debugfs: unlink <文件名>          # 或 kill_file <inode>
debugfs: ls /                     # 文件名消失(inodes 那一列变 0 或整项不见了)
debugfs: show_super_stats         # Free inodes count、Free blocks count 应回升
```

核对回收效果:

- 根目录里那一项没了(或它的 inode 字段被清 0,留空洞——对应内核 `remove_dir_entry` 对「块内首项」的处理)。
- 该文件 inode 在 inode 位图里被释放:`Free inodes count` 回升 1。
- 该文件占的数据块在块位图里被释放:`Free blocks count` 回升。
- (可选)`testi <原 inode>` 应报告「未使用」。

**对应到内核**:目录项移除是 `remove_dir_entry`;inode/块释放是 `Ext2::unlink` 在 `i_links_count` 归零时,遍历直接块(0..11)和单间接块逐个 `free_block`,最后 `free_inode`。注意主书指出的不对称:**内核的 `unlink` 能释放单间接块,但 `write` 写不出单间接块**——能删的,写不进去。`debugfs` 这边没有这个限制,所以你在 `debugfs` 里能造出内核自己写不出来的大文件,别拿来反推内核能力。

> 特别留意:**目录自身的「数据块」不会被回收**。即使你把一个目录里删空了,它占的那几个数据块还挂着。内核 `remove_dir_entry` 同样不回收目录块(主书「目录项增删」一节)。这是 028b 一个有意的简化,不是 bug——但你要知道它在那里。

### 任务 5(边界):写一个「太大」的文件

主书讲过 `Ext2FileOps::write` 有条会截断的循环:`if (file_block > EXT2_DIRECT_BLOCKS) break`,把写限制在逻辑块 0..12(block_size=1024 时约 13KB)。

这个任务**不在 `debugfs` 里做**(`debugfs` 的 `write` 不受此限),而是要你在内核行为里验证它。见下面「验证步骤」的加分项。先记住结论:内核写大文件会被静默截断,`write` 返回值小于请求字节数。

## 接口约束

下面是 Cinux ext2 驱动在 028b 暴露的、和「写」相关的接口职责。lab 里你用 `debugfs` 做的每一步,内核里都由其中某个方法负责——对照着看,别只背函数名。

- `Ext2::alloc_inode()` / `alloc_block()`:遍历块组、读位图、找空闲位、置位、写回位图,并同步 `s_free_inodes_count`/`s_free_blocks_count`(超块)和 `bg_free_inodes_count`/`bg_free_blocks_count`(组描述符)。返回 0 = 失败(盘满)。
- `Ext2::free_inode(ino)` / `free_block(blk)`:上述的逆操作,清位图位、计数 +1、写回。
- `Ext2::write_disk_inode(ino, inode)`:**read-modify-write**——读出 inode 所在块,覆盖那一个 inode,整块写回。
- `Ext2::add_dir_entry(...)`:在父目录数据块里插一项,优先 split 现有项的 `rec_len`,不够再 `alloc_block` 新块;目录只走直接块(≤12 块)。
- `Ext2::remove_dir_entry(...)`:首项 inode 清 0 留空洞,否则把 `rec_len` 并入前一项;**不释放目录数据块**。
- `Ext2FileOps::write(inode, offset, buf, count)`:逐块 `get_or_alloc_block` 取/分配块,部分块 read-modify-write,更新 `i_size`/`i_blocks`,`write_disk_inode`;`file_block > 12` 时 break(截断)。
- `Ext2::create/mkdir/unlink`:编排上面这些原语,负责**失败回滚**(每步分配都要配一个释放)。
- `InodeOps` 的 `write`/`create`/`mkdir`/`unlink` 四个虚方法是 028b 新增的(028 只有 `read`/`readdir`)。`Ext2FileOps` 实现 `read`/`write`,`Ext2DirOps` 实现 `readdir`/`create`/`mkdir`/`unlink`。

## 验证步骤

**host 路径(必做)**:按任务 1–4 操作,每步前后用 `stat`/`ls`/`show_super_stats` 记录,确认变化方向符合「接口约束」的描述。退出 `debugfs` 用 `quit`(或 `q`),别用 `Ctrl-C`(可能不落盘)。

退出后,可以用 `fsck` 给你的「实验成果」做个体检——它正是 ext2 一致性的裁判:

```bash
e2fsck -fn /tmp/lab.ext2     # -f 强制检查, -n 只读不修
```

如果你的操作都规整(`debugfs` 本身会维护计数一致性),`e2fsck` 应该报「clean」。这条命令也是你将来排查「内核写出来的盘有没有坏」的标准手段。

**加分路径(端到端)**:在跑起来的 Cinux shell 里(用项目提供的带交互 shell 的运行方式;若没有,可读 `kernel/test/test_shell_write.cpp`、`test_syscall_ext2.cpp` 看测试如何驱动 shell 命令),做一串操作并对照:

```text
$ touch /e2e.txt
$ echo from-cinux > /e2e.txt
$ mkdir /e2edir
$ (把 /e2e.txt 读回来)          # 应得到 "from-cinux"
$ rm /e2e.txt
$ rmdir /e2edir
```

任务 5 的截断验证也在这里:想办法往一个文件写超过 13KB(比如多次 `echo` 拼接,或写一个大点的 seed 文件),观察 `write` 是否只写进去一部分。能观察到「写了但没写全」,就证实了那条 break。

## 常见故障

- **「我什么都没动,`debugfs` 看到的数据和上次不一样」**:八成是看了 CI 那份被 `run-kernel-test` regenerate 过、又被测试写脏的镜像。lab 一律用你自己的 `/tmp/lab.ext2` 拷贝。
- **「建完文件,`Free inodes count` 没变」**:忘了它。`debugfs` 自己会同步,但你要是在**改内核代码**做实验,这就是最经典的漏:`alloc_inode` 改了位图却没 `write_superblock()`/`write_bgdt()`。位图是分配的真正依据(能继续分),但计数漂移会让「这组满了就跳过」的判断出错,后面分到错的组。三件套(位图 + 超块计数 + BGDT 计数)一个都不能少。
- **「目录项乱码 / `ls` 报错」**:`rec_len` 不是 4 的倍数。ext2 目录项长度必须 4 字节对齐(主书「目录项增删」)。`debugfs` 会帮你对齐,内核里 `add_dir_entry` 也要 `(rec_len + 3) & ~3u`。
- **「部分写之后,文件别处的内容没了」**:写一个没对齐到块边界的区间时,没先 `read_block` 把原块读出来就直接覆盖。`Ext2FileOps::write` 里 `block_offset != 0 || chunk != bs` 那个分支就是防这个的。
- **「我想验证持久化,但每次都变」**:`run-kernel-test` 每次前 `regenerate-ext2-image`。要观察「跨重启持久」,得用同一个镜像跑两次、中间不 regenerate(手动跑 QEMU 时自己控制)。
- **`rmdir` 一个非空目录被拒**:这是对的。`sys_rmdir` 用 `readdir(target, 2)`(跳过 `.`/`..`)检测非空,非空就拒。内核 `Ext2::unlink` 本身不查空——绕过 `sys_rmdir` 直接调 `unlink` 会删掉非空目录(主书警告过的「注释漂移」)。

## 通过标准

- 任务 1–4 里,每一次操作的「位图位翻转 / inode 出现或消失 / 空闲计数 ±1 / links 计数变化 / 目录项增删」都对得上「接口约束」的预期。
- 能口头把 `debugfs` 的每一步对应到内核的一个方法(比如「`mkdir sub` 让根 links +1,对应 `Ext2::mkdir` 里的 `dir_disk.i_links_count++`」)。
- 能解释三个为什么:(a) 为什么建一个目录,父目录的 `links` 要 +1;(b) 为什么 `rm` 一个文件后 `Free inodes` 会回升、但「目录自己的数据块」不一定会被回收;(c) 为什么内核写不出大文件、却删得掉大文件。
- `e2fsck -fn` 对你操作过的镜像报 clean(说明你维护的一致性和 ext2 标准一致)。
- (加分)在真 Cinux shell 里端到端走通,且能复现任务 5 的写截断。
