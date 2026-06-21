---
title: Lab 028c · 路径规范化与 stat:把相对路径和文件信息跑通
---

# Lab 028c · 路径规范化与 stat:把相对路径和文件信息跑通

> 028c 给文件系统加了工作目录和 stat。这个 lab 分两半:前半是「纸上练兵」——`path_canonicalize` 是个纯字符串函数,特别适合在 host 上拿一批刁钻输入去喂它、断言结果,把 `.`/`..`/根目录保护这些边界彻底搞懂;后半是「真刀真枪」——在跑起来的 Cinux 里 `cd`/`pwd`/`stat`,再用 `debugfs` 对照 stat 读出来的数字和磁盘上的是不是一致。最后留一个整洁度改进任务:把 ext2 里两份一模一样的 `stat` 收成一个。

## 实验目标

- 用 host 单测驱动 `path_canonicalize` / `path_resolve`,覆盖 `.`、`..`、重复斜杠、**根目录越过**等边界,亲手验证「`cd /..` 还是 `/`」。
- 在 Cinux shell 里端到端走 `cd`/`pwd`/`stat`,确认相对路径解析、工作目录记忆、文件信息查询都正确。
- 用 `debugfs` 对照 `sys_stat` 返回的 `st_size`/`st_ino`/`st_mode` 与磁盘 inode 的真实值,理解「stat 字段从哪来、哪些是空的」。
- (改进)消除 `Ext2FileOps::stat` 与 `Ext2DirOps::stat` 的重复。

## 前置条件

- 028b 的写能力可用(`touch`/`echo >`/`mkdir` 能跑)。
- 028c 的代码已构建:`cmake --build build`,且 `ctest --test-dir build -R cwd_stat` 能跑。
- host 有 `debugfs`(e2fsprogs)。准备一块干净镜像:`./scripts/create_ext2_disk.sh /tmp/lab.ext2`。
- 读懂主书第 028c 章的「路径规范化」和「struct stat 与 InodeOps::stat」两节。

## 任务分解

### 任务 1:用 host 单测压测 path_canonicalize

`path_canonicalize(char* buf)` 是纯函数:输入一个(可能乱七八糟的)路径字符串,就地改成规范绝对路径。它不碰磁盘、不查存在性,纯字符串处理——正是单测的好材料。

参考 `test/unit/test_cwd_stat.cpp` 的纯函数测试段,自己构造一组输入,对每个断言规范化结果。重点覆盖这几类(括号里是预期输出):

```text
/a/b/../c            → /a/c        (.. 弹一层)
/a/./b//c            → /a/b/c      (. 跳过,// 折叠)
/..                  → /           (根的 .. 仍是根 —— 重点!)
/a/b/../../..        → /           (连续 .. 不能越过根)
/                    → /           (单独的根)
/a/                  → /a          (尾斜杠去掉)
/a/../..             → /           (弹到根再弹,仍根)
//a//b//             → /a/b        (全是重复斜杠)
```

关键是后三条:**任何情况下结果都不该小于 `/`,也不该出现 `out_pos` 为负或负数下标**。如果你把 `path_canonicalize` 里的根目录保护(`if (out_pos > 1)`)故意去掉,重新跑这组用例,你应该能看到 `/..` 或 `/a/../..` 产生错误结果(比如空串、或越界)——这就反证了那行保护的价值。

> 接口约束:`path_canonicalize` 入参 `buf` 必须可写、长度 < `PATH_MAX`(4096);返回值无(就地修改);对空串或 nullptr 直接返回不改。

### 任务 2:path_resolve 的相对/绝对分支

`path_resolve(cwd, path, out)` 有两条分支:`path` 以 `/` 开头走绝对(直接拷贝+规范化),否则走相对(拼 `cwd + "/" + path` 再规范化)。构造用例覆盖:

```text
cwd="/etc",  path="motd"        → /etc/motd
cwd="/etc",  path="/hello.txt"  → /hello.txt      (绝对优先)
cwd="/",     path="etc/motd"    → /etc/motd
cwd="/a/b",  path="../c"        → /a/c            (相对路径里的 ..)
cwd="/a/b/", path="c"           → /a/b/c          (cwd 带尾斜杠,别拼出 //)
```

注意最后一条:cwd 以 `/` 结尾时,拼接逻辑要避免出现 `//`(主书讲过 `path_resolve` 会判断 cwd 是否已有尾斜杠)。验你的实现不会拼出双斜杠。

### 任务 3:端到端 cd / pwd

在跑起来的 Cinux shell 里(带交互 shell 的运行方式),验证工作目录真的被记住:

```text
$ pwd                  → /
$ cd etc
$ pwd                  → /etc
$ cd ..                
$ pwd                  → /        (.. 在 shell 里要能正确解析;注意 shell 怎么把 ".." 传给 sys_chdir)
$ cd /etc/nonexistent  → 应被拒(sys_chdir 的目录类型/存在性检查)
$ pwd                  → 仍是上一个有效 cwd(失败的 cd 不该改 cwd)
```

要核对的点:**失败的 `cd`(目标不存在或不是目录)不能改变当前 cwd**。看 `sys_chdir` 的实现——它在 lookup 失败和类型检查失败时都是 `return -1`,没动 `current->cwd`。你的 shell 行为应符合这一点。

### 任务 4:stat 与 debugfs 对照

这是把「内核 stat」和「磁盘真相」对齐的环节。先用 `debugfs` 在干净镜像上记下几个文件的 inode 信息:

```bash
debugfs -R "stat <hello.txt 的 inode>" /tmp/lab.ext2     # 记下 size、mode、links
debugfs -R "ls -l /" /tmp/lab.ext2                       # 记下各文件的 inode 号
```

然后在 Cinux 里对同一个文件 `stat`(具体 shell 的 `stat` 命令格式以 `cmd_stat.cpp` 为准),对比:

- `st_size` 应等于 `debugfs stat` 显示的 size。
- `st_ino` 应等于 `debugfs ls -l` 显示的 inode 号。
- `st_mode` 的类型位(目录 `0x4000`/普通文件 `0x8000`)应与文件实际类型一致。
- `st_nlink` 应等于 inode 的 links count(普通文件通常 1,目录至少 2)。
- `st_blksize` 应是 ext2 的 block size(1024)。
- **`st_dev`、`st_rdev` 应是 0**(Cinux 无设备号)。
- **三个时间戳应是 0**(无 RTC)——别被「全是 1970」吓到,这是预期。

如果内核 stat 打出的 size 和 debugfs 不一致,要么 lookup 拿错了 inode,要么 stat 读错了 `disk_inode`——回去查。

### 任务 5(改进):给 stat 去重

主书指出,`Ext2FileOps::stat` 和 `Ext2DirOps::stat` 逐字相同。这是个明显的重复。你的任务:把这段公共逻辑提到一个地方,让两个 Ops 类都复用,且不改变行为。

可选做法(任选其一,想清楚取舍):

- 在 `Ext2` 类里加一个私有 helper(比如 `fill_stat_from_inode(const Inode*, struct stat*)`),两个 `stat` 都调它。
- 或者在基类 `InodeOps` 里给 `stat` 一个默认实现(因为 ext2 的文件和目录 stat 完全一样),让两个派生类不再 override——但要想清楚:别的文件系统后端(ramdisk)会不会需要不同的 stat?默认实现会不会反而限制了扩展?

改完重跑 `ctest -R cwd_stat`,确认行为不变。这是个纯重构,测试不该有任何变化。

## 接口约束

这一章涉及的关键接口,lab 里你测的每个点都对应其中之一:

- `cinux::fs::path_canonicalize(char* buf)`:就地规范化,结果恒为绝对路径;不查存在性。
- `cinux::fs::path_resolve(cwd, path, out)`:绝对优先,相对拼 cwd;输出缓冲至少 `PATH_MAX`。
- `cinux::syscall::validate_user_ptr(uint64_t)`:canonical 地址校验(inline)。
- `cinux::syscall::resolve_user_path(path_virt, out)`:校验 + 取 `Scheduler::current()->cwd`(nullptr 退回 "/")+ path_resolve。
- `cinux::proc::Task::cwd[256]`:per-process 工作目录,绝对路径,定长 256。
- `cinux::proc::Scheduler::current()` / `set_current(Task*)`:取/设当前进程。
- `cinux::fs::InodeOps::stat(const Inode*, struct stat*)`:第 7 个虚方法,后端把磁盘 inode 翻译成 stat。
- `sys_stat`/`sys_fstat`/`sys_chdir`/`sys_getcwd`:号 4/5/12/79。

## 验证步骤

- **任务 1–2**:在 `test/unit/test_cwd_stat.cpp` 里(或新建一个测试文件)加你的用例,`ctest --test-dir build -R cwd_stat --output-on-failure` 全绿。故意去掉根目录保护、重跑,确认 `/..` 类用例**变红**——反证保护有效,然后改回来。
- **任务 3–4**:用带交互 shell 的 Cinux 运行(或读 `kernel/test/test_cwd_stat.cpp` 看它怎么驱动 shell 命令),手敲命令;`stat` 结果与 `debugfs` 逐一对照。
- **任务 5**:`cmake --build build && ctest --test-dir build -R cwd_stat`,全绿且无行为变化。
- 全程用你自己的 `/tmp/lab.ext2` 拷贝,别动 CI 那份(会被 `regenerate-ext2-image` 冲掉)。

## 常见故障

- **`cd /..` 或 `cd a/../../..` 后路径乱了 / 内核崩了**:`path_canonicalize` 漏了根目录保护(`if (out_pos > 1)`)。`..` 在根时应是 no-op,漏了会越界写 `out[]`。
- **相对路径永远被当绝对路径 / 永远相对 "/"**:`resolve_user_path` 没取到 current task 的 cwd(要么 `Scheduler::current()` 返回 nullptr,要么 `launch_first_user` 没 `set_current`)。症状是 `cd etc` 后 `pwd` 还是 `/` 或解析错位。
- **`cd` 失败后 cwd 变了**:`sys_chdir` 在 lookup/类型检查失败前就改了 `current->cwd`。正确顺序是「全检查通过才写 cwd」。
- **stat 的 size 对不上**:多半是 lookup 拿到了错的 inode(路径解析错),或者 stat 读的 `disk_inode` 不是这个 inode 的。先 `pwd`/`debugfs` 双向核对路径。
- **stat 时间是 0,以为坏了**:不是 bug。Cinux 无 RTC,`st_atime/mtime/ctime` 取自磁盘 inode(全 0)。`st_dev`/`st_rdev` 同理是 0。
- **`getcwd` 返回 -1 但缓冲明明够**:看 `sys_getcwd` 的长度判断——它算的是「含 NUL 的长度」,你的缓冲 size 至少要 `strlen(cwd)+1`。
- **改了 stat 去重后测试挂了**:重构动了行为。纯重构不该改变任何输出,逐字段核对你的 helper 和原实现是否一字不差。

## 通过标准

- 任务 1–2 的所有规范化/解析用例通过,且能解释「为什么 `/..` 必须是 `/`」。
- 任务 3 里 `cd`/`pwd` 行为正确,**失败的 cd 不改变 cwd**。
- 任务 4 里 `stat` 的 `st_size`/`st_ino`/`st_mode`/`st_nlink`/`st_blksize` 与 `debugfs` 完全一致;`st_dev`/`st_rdev`/时间戳为 0 且你能解释原因。
- 任务 5 完成去重,`ctest -R cwd_stat` 全绿、行为不变。
- 能口头回答:相对路径是怎么变成绝对路径的?cwd 挂在哪里?为什么第一个用户进程要 `set_current`?
