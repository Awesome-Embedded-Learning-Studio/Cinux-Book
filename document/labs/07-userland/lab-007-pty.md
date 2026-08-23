---
title: Lab 007 · PTY 验证
---

# Lab 007 · PTY 验证

> 对应 `document/book/07-userland/007/`。验证档 **A 档**:这一章交付的是 Linux PTY ABI(/dev/ptmx 克隆 + /dev/pts/N + master/slave + TIOCSCTTY)。可「真用户程序跑在 PTY 里」的全闭环要 dup2 + 真 shell,留 CFBox——这一章的验证靠 host 纯逻辑单测 + 内核设备测 + boot 冒烟,辅以改公共头 inode.hpp 的回归网。

## 目标

确认七件事:

1. **PTY 核心是纯逻辑四数据路径**(master 写/读 + slave 读/写),host 单测 10 例;
2. **slave 复用 005 的 TTY 行规范**(不重写),回显经 echo sink 路由回 master 读侧;
3. **InodeOps 加了 ioctl/open 两个 virtual**(对齐 Linux fops),默认 ioctl→NotImplemented→ENOTTY;
4. **sys_ioctl 对 fd>2 走 inode 派发**(fd≤2 console 零变);
5. **/dev/ptmx 克隆 open**(分配一对 PTY 返 master)+ **/dev/pts/N 动态查找**;
6. **TIOCSCTTY 挂控制终端 + /dev/tty 别名**;
7. **boot 真加载 PTY 节点**:`make run` 见 `[DEVFS] mounted ... (4 nodes)`(多一个 ptmx)。

## 步骤

### 1. host 单测:PTY 纯逻辑

```bash
./build/test/test_pty
```

应看到 `pty tests OK (10 cases)`。十例罩的是 `Pty` 类四数据路径:canonical 行 round-trip(master "hi\n" → slave 读 "hi\n")、本地回显(master 写后 master_read 看到)、slave 输出 → master、`^C` → pending SIGINT、raw 模式、退格编辑、空行 `^D` EOF、行内 `^D` 提交、满环 partial write、双实例不串扰。这层能 host 单测是因为 `pty.cpp` 纯逻辑(只依赖同样纯逻辑的 `TTY`)。

### 2. PTY 核心:四数据路径

```bash
sed -n '47,70p' kernel/drivers/tty/pty.hpp
```

应看到 `class Pty` 的四个方法:`master_write`(每字节喂 slave `TTY::input_char`)、`master_read`(排 master 读环)、`slave_read`(排 `TTY::read_cooked`)、`slave_write`(push master 读环)。文件头注释会画那张四路径表。注意 slave 持一个复用的 `TTY`(005 那块行规范),PTY 的终端感全来自它——不重写。

### 3. 两条新接缝:InodeOps::ioctl / open

```bash
sed -n '68,80p' kernel/fs/inode.hpp
```

应看到 `InodeOps` 加了两个 virtual:`ioctl(inode, request, arg)`(默认 NotImplemented,注释说 sys_ioctl 特判 → ENOTTY)、`open(inode)`(默认返原 inode,PTY 的 `/dev/ptmx` override 成克隆)。这是对齐 Linux fops(`unlocked_ioctl`/`open` 克隆)的两条缝。

### 4. sys_ioctl fd>2 走 inode 派发

```bash
sed -n '130,145p' kernel/syscall/sys_ioctl.cpp
```

应看到 fd≤2 走 `console_ioctl`(行为零变),fd>2 走 `current_fd_table().get(fd) → file->inode->ops->ioctl(...)`,失败时 `NotImplemented → -ENOTTY`(特判,不是 ENOSYS),无 File 也 `-ENOTTY`(保 `test_sys_ioctl_non_tty_fd_enotty` 契约)。

### 5. /dev/ptmx 克隆 + /dev/pts/N

```bash
sed -n '211,240p' kernel/drivers/tty/pty_device.cpp
```

应看到 `PtmxOps : public InodeOps`,`open` override 调 `pty_alloc()`(找空槽)+ `wire_slot()`(把 master/slave inode 的 ops 指向共享 ops、`fs_private = &slot.pty`)+ 返 `g_slots[idx].master_inode`。这是 Linux `/dev/ptmx` 克隆范式。`/dev/pts/N` 看 DevFs 的动态查找:

```bash
grep -n "set_dynamic_lookup\|pts/\|pty_slave_inode" kernel/fs/devfs_init.cpp kernel/drivers/tty/pty_device.cpp | head
```

应看到 DevFs 的 `set_dynamic_lookup` 回调(resolver 解析 `pts/<N>` → `pty_slave_inode(N)`)。DevFs 自己不认 PTY(保持 host 可测);PTY 装配只在 kernel-only 的 `devfs_init.cpp`。

再看注册表:

```bash
sed -n '38,52p' kernel/drivers/tty/pty_device.cpp
```

应看到 `kPtyInoBase = 0x1000`(inode 号编码 pty 索引,`ino = base + index`,`TIOCGPTN` 返 `ino - base`)+ `PtySlot g_slots[kMaxPtys]`(8 槽固定表,inline 拥有 Pty + master/slave Inode)。

### 6. TIOCSCTTY + /dev/tty(控制终端)

```bash
grep -n "TIOCSCTTY\|controlling_tty\|kTiocsctty\|/dev/tty" kernel/drivers/tty/pty_device.cpp kernel/fs/devfs_init.cpp | head
```

应看到 `TIOCSCTTY` 的 slave inode ioctl override(设 `Task::controlling_tty`)+ `/dev/tty` 的 dynamic lookup(返当前 task 的 controlling_tty 对应 inode)。这把 PTY 跟 session/前台组/信号(005 那套)接通。

### 7. kernel 设备测 + boot 冒烟

```bash
cmake --build build --target run-kernel-test 2>&1 | grep -iE "pty" | head
```

应看到 `test_pty_device` 几项 PASS(alloc/slave-lookup、master↔slave round-trip、本地回显、termios 接线、未知 ioctl 拒、release 复用)+ TIOCSCTTY 两例(session leader 挂上 / 非-leader EACCES)。

boot 冒烟(test kernel 不走 `devfs::init`,要起真内核):

```bash
cmake --build build --target run
```

应看到 `[DEVFS] mounted at /dev (4 nodes)`——比 064(3 nodes)多一个 `ptmx`。

两腿汇总:

```bash
cmake --build build --target run-kernel-test-all 2>&1 | grep -E "Tests: 9[0-9][0-9] passed" | tail -1
```

应看到单核 + `-smp 2` 两腿各 `986 passed, 0 failed`。

## 验收清单

- [ ] `./build/test/test_pty` 报 10 cases OK(PTY 纯逻辑四数据路径)。
- [ ] `pty.hpp:47` `class Pty` 四方法(master_write/read + slave_read/write),slave 复用 `TTY` 行规范。
- [ ] `inode.hpp:72` `ioctl` virtual(默认 NotImplemented→ENOTTY)、`:79` `open` virtual(默认原 inode)。
- [ ] `sys_ioctl.cpp:135` fd>2 走 `file->inode->ops->ioctl`,NotImplemented→ENOTTY;fd≤2 console 零变。
- [ ] `pty_device.cpp:211` `PtmxOps::open` 克隆(pty_alloc + wire_slot + 返 master);`:52` `g_slots[kMaxPtys=8]`;`:38` `kPtyInoBase=0x1000`。
- [ ] `/dev/pts/N` + `/dev/tty` 经 DevFs `set_dynamic_lookup` 回调(devfs_init 装,devfs.cpp 不碰 PTY)。
- [ ] `make run` 见 `[DEVFS] mounted ... (4 nodes)`(含 ptmx);两腿 986/0。

## 别做这些

- **别**指望用户态程序真跑在 PTY 里——全闭环要 `sys_dup2`(把 slave 重定向成子进程 stdio,**当前未实现**)+ 真 session + 一个会 `open("/dev/ptmx")` 的 shell。这一章把 PTY **机制**做全了,「真程序跑 PTY」留 CFBox。ring0 测试也不能调带 user 指针的 syscall(`access_ok` 拒内核址),所以纯 ring0 syscall smoke 做不了。
- **别**以为 slave read 会阻塞——现在非阻塞(0 = 无数据,EOF 经 `take_eof` 分)。真 shell 读 stdin 要阻塞(睡等行),要 reader Task + 唤醒,留 follow-up。
- **别**以为 close 会释放 PTY——现在没 close→release 钩子(槽位靠测试显式 release)。生产用要补。
- **别**纠结 errno 不完全对齐 Linux——copy 失败返 EINVAL(Linux EFAULT)、TIOCSCTTY 拒绝返 EACCES(Linux EPERM)。Cinux-Base 的 Error 枚举所限(彼时子模块边界不擅改,现已并回 `libs/base`),行为对、errno 近似,已登记。
- **别**以为能开无限多 PTY——固定 8 槽(`kMaxPtys`)。够当前用,动态扩展留后。
- **别**把 fd≤2 也当成走 inode 派发——0/1/2 是 console 特殊路径(不是真 FDTable 项),跟 fd>2 的 PTY 派发是两条路。统一它们要连带改 sys_read/sys_write,爆炸半径大,不做。
