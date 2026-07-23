---
title: Lab 064 · DevFS 验证
---

# Lab 064 · DevFS 验证

> 对应 `document/book/08-filesystem/064-devfs.md`。验证档 **A 档**:这一章交付的是 `/dev` 真挂上、`ls /dev` 见设备、读写触发设备行为。核心逻辑靠 host 单测 + kernel 内测试罩,boot 真挂 `/dev` 靠 `make run` 冒烟(因为 test kernel 不走 boot 的 `devfs::init()`)。

## 目标

确认六件事:

1. **device inode 是 InodeOps 子类**:Null/Zero/Console/Dir 四个,基类接口一行不动;
2. **设备号收进 ops 子类**(`st_rdev` 由 `stat()` override 填,不加 `Inode` 字段);
3. **CharSink 解耦**:console 设备只认 `CharSink`,内核注入真串口、host 注入 mock,核心逻辑 host 可测;
4. **三个基础节点行为对**:null(1:3 写丢/读 EOF)、zero(1:5 读零/写丢)、console(写打串口);
5. **boot 真挂 /dev**:`make run` 见 `[DEVFS] mounted`,`ls /dev` 见 null/zero/console;
6. **§14 文件 gate**:devfs.cpp(host 可链)与 devfs_init.cpp(kernel-only)分离,源码零 `#ifdef`。

## 步骤

### 1. host 单测:核心逻辑

```bash
./build/test/test_devfs
```

应看到 `19 passed, 0 failed`。十九个 case 罩的是核心:mount 建三节点、`lookup("null"/"zero"/"console")` 命中对的 inode、null 读 EOF 写丢、zero 读零写丢、console write 走 mock sink(断言字节对)、stat 填对 `st_rdev`/`st_mode`、`/dev` 目录 readdir 列节点、mount 幂等(重复不累加)。这层靠 `CharSink` 解耦——mock sink 让 console 派发在 host 可断言。

### 2. device inode:InodeOps 子类

```bash
sed -n '47,130p' kernel/fs/devfs.cpp
```

应看到匿名 namespace 里四个 `class XxxDevOps : public InodeOps`:`NullDevOps`(read 返 0=EOF,write 返 count 假装全写)、`ZeroDevOps`(read memset 零,write 丢)、`ConsoleDevOps`(持 `CharSink*`,write 调 `sink_->write`)、`DevDirOps`(/dev 目录的 readdir)。注意每个的 `stat()` override 填 `st_rdev = devfs_makedev(major, minor)`(null 1:3、zero 1:5)和 `st_mode = kSIfChr|0666`。

确认基类接口没动:

```bash
grep -nE "virtual.*read|virtual.*write|virtual.*stat|class InodeOps" kernel/fs/inode.hpp | head
```

`InodeOps` 的虚函数签名跟 ext2/ramdisk 用的是同一套——DevFS 只加子类,没改基类。这就是「加新文件类型不动基类」的并行栅栏。

### 3. CharSink 解耦

```bash
sed -n '57,66p' kernel/fs/devfs.hpp
```

应看到 `class CharSink`(纯虚 `write(buf, count) → ErrorOr<int64_t>`)。再看内核注入的真 sink:

```bash
sed -n '34,55p' kernel/fs/devfs_init.cpp
```

应看到 `SerialConsoleSink : public CharSink`(write 转成 COM1 逐字节 `putc`,照 kprintf 的 serial sink)+ 静态 `g_devfs_sink` / `g_devfs{&g_devfs_sink}`。核心(devfs.cpp)只认 `CharSink*`,不知道背后是串口;真串口依赖被隔离在 devfs_init.cpp(kernel-only)。host 单测注入 mock sink,所以 host 能链 devfs.cpp 而不碰 Serial。

### 4. §14 文件 gate

```bash
grep -rnE "#ifdef|#ifndef" kernel/fs/devfs.cpp kernel/fs/devfs_init.cpp | head
```

应看到**两个文件里都没有** `#ifdef CINUX_HOST_TEST` 之类的条件编译。能不能 host 链是 CMake 决定的:devfs.cpp 进 host 测,devfs_init.cpp(kernel-only)不进。源码零 `#ifdef`,这是「同一特性按 host-可链拆两文件」的 §14 套路。确认 CMake 的分流:

```bash
grep -nE "devfs" kernel/fs/CMakeLists.txt kernel/CMakeLists.txt
```

应看到 devfs.cpp 进 `big_kernel_common`(内核 + host 都链),devfs_init.cpp 只进 kernel target(不进 host)。

### 5. boot:挂上 /dev

```bash
sed -n '49,49p' kernel/proc/init.cpp
```

应看到 ext2 挂 `/` 之后调 `cinux::fs::devfs::init()`(boot 装配一行)。看 init 干了啥:

```bash
sed -n '55,70p' kernel/fs/devfs_init.cpp
```

应看到 `devfs::init()`:`g_devfs.mount()`(建标准节点)+ `vfs_mount_add("/dev", ...)`(注册到 VFS mount 表)。

**冒烟**(这一步 test kernel 罩不到,要起真内核):

```bash
cmake --build build --target run
```

boot 序列应打 `[VFS] ext2 mounted at /` → `[DEVFS] mounted at /dev (3 nodes)`。进 shell 后:

- `ls /dev` 见 `null`、`zero`、`console`;
- `echo hi > /dev/null`(null 吃掉,不报错);
- `head -c 4 /dev/zero | xxd`(读出 `00000000`,zero 给零)。

### 6. kernel 内 test_devfs

```bash
cmake --build build --target run-kernel-test 2>&1 | grep -iE "devfs" | head
```

应看到 `test_devfs::test_null_read_eof` 等七项 PASS——内核态跑同一套核心逻辑,证它在内核环境也对(boot 接线那部分 test kernel 不走,这里不覆盖)。

两腿汇总:

```bash
cmake --build build --target run-kernel-test-all 2>&1 | grep -E "Tests: 9[0-9][0-9] passed" | tail -1
```

应看到单核 + `-smp 2` 两腿各 `976 passed, 0 failed`。

## 验收清单

- [ ] `./build/test/test_devfs` 报 19 passed。
- [ ] `devfs.cpp:47` 起 Null/Zero/Console/Dir 四个 `InodeOps` 子类;`inode.hpp` 的 `InodeOps` 虚函数签名没动(只加子类)。
- [ ] 各子类 `stat()` override 填 `st_rdev = devfs_makedev(major,minor)`(null 1:3、zero 1:5)+ `st_mode = kSIfChr|0666`;`Inode` 结构体没加字段。
- [ ] `devfs.hpp:57` `CharSink` 接缝;`devfs_init.cpp:34` `SerialConsoleSink` 注入;host 测注入 mock → console 派发 host 可断言。
- [ ] devfs.cpp / devfs_init.cpp **零 `#ifdef`**;CMake 把 devfs_init.cpp 排除出 host 测。
- [ ] `make run` 见 `[DEVFS] mounted at /dev (3 nodes)`;`ls /dev` 见 null/zero/console;两腿 976/0。

## 别做这些

- **别**以为 run-kernel-test 验了「boot 挂 /dev」——test kernel 走 `main_test.cpp`,不调 `devfs::init()`。boot 真挂 `/dev` 要 `make run` 起真内核看 `[DEVFS] mounted`。run-kernel-test 只证 devfs_init.cpp link 进 test kernel 没破 + 核心/kernel test_devfs 逻辑对。
- **别**以为 `/dev/console` 能读——这一章只接了**写**(到串口)。读(接 062 console TTY 的真 stdin)、`/dev/tty`、PTY 是 TTY Phase 2 的事。`/dev/null`/`/dev/zero` 是完整的,console 这半先把写做对。
- **别**以为能用户态 `mknod` 造设备——设备是 boot 时 `mount()` 预建的固定三个(null/zero/console)。mknod、动态设备节点留后面。
- **别**给 `Inode` 加 `st_rdev` 字段——设备号收在 ops 子类的 `stat()` override 里。改基类字段会让所有 `InodeOps` 子类(ext2/ramdisk/DevFS)都跟着动,破坏并行。这是「加东西靠子类不动基类」的纪律。
- **别**把 16 个 DevNode 槽当限制——那是基础节点够用的数,要做更多设备(PTY 那一堆 `/dev/pts/N`)再扩。
