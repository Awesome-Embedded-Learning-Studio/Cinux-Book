---
title: 02 · 代码路线:PTY 核心、两条接缝、DevFS 节点与控制终端
---

# 代码路线:PTY 核心、两条接缝、DevFS 节点与控制终端

## PTY 核心:四条数据路径,复用 062 的行规范

PTY 核心是一个 `Pty` 类(`pty.hpp:48`),四条数据路径:

| 路径 | 干什么 |
|------|--------|
| `master_write` | 每字节喂 slave 的 `TTY::input_char`(行规范:canonical 编辑/回显/信号) |
| `slave_read` | 排 `TTY::read_cooked` 已经 commit 的 cooked 行 |
| `slave_write` | 程序输出 → push 进 master 读环(环满 partial write) |
| `master_read` | 排 master 读环(slave 的输出 + 本地回显) |

(`pty.hpp:58` 起。)两个关键设计。

**其一,slave 复用 062 的 `TTY`,不重写。** PTY 的「终端感」全来自 slave 侧的行规范——攒行、退格、Ctrl+C 翻译信号,这些 062 那个纯逻辑 `TTY` 全做了(host 可测、带 echo sink 注入)。PTY 直接拥有一个 `TTY` 当 slave侧行规范,等于白捡一整套终端行为。这跟前面反复出现的「纯逻辑 + 注入式解耦」一脉相承:`TTY` 当初写成纯逻辑、host 可测,现在换个场景(PTY)直接复用,不用重写一遍。

**其二,回显路由回 master 读侧。** 你在模拟器里敲一个字,期望屏幕上立刻看到它(本地回显)。PTY 怎么做到?slave 的 `TTY` 有个 echo sink(062 立的接缝),构造 PTY 时把 echo sink 接到「往 master 读环 push」上——于是 master 写一个字节 → slave 行规范处理 → 回显经 sink 回到 master 读环 → 模拟器从 master 读到那个字节。回显 best-effort:master 读环满了就丢回显字节,不阻塞输入路径。信号字符(`^C`)同理:行规范认出来记成 `pending_signal`,`Pty::take_pending_signal()` 转发,内核侧(后面)投给 slave 的前台进程组。

> 这一批(`pty.cpp`)又是**纯逻辑**:零 `kprintf`、零 `proc`、零 Console,能直接链进 host 单测。`test/unit/test_pty.cpp` 十个 case:canonical 行 round-trip、本地回显、slave 输出 → master、`^C` → pending SIGINT、raw 模式、退格编辑、空行 `^D` EOF、行内 `^D` 提交、满环 partial write、双实例不串扰。能 host 单测,是因为它只依赖 `TTY`(也是纯逻辑),不碰任何内核东西。

## 两条接缝:让 PTY 接进 fd

PTY 核心是纯对象,没接 fd。要让一个进程拿到 PTY 的 master fd 并对它 ioctl(拿 termios、挂控制终端),得先有两条基础设施——这俩之前都缺。

**缝一:`InodeOps::ioctl` virtual。** 之前 `InodeOps` 虚表里**没有 ioctl 方法**(063 的 ioctl 只处理 fd≤2 的 console)。PTY 的 fd>2,得让设备 inode 能响应 ioctl。于是给 `InodeOps` 加 `ioctl` 虚函数(`inode.hpp:105`),默认返 `NotImplemented`:

```cpp
virtual ErrorOr<int64_t> ioctl(const Inode* inode, uint32_t request, uint64_t arg);
// 默认返 NotImplemented;sys_ioctl 特判 → -ENOTTY
```

**缝二:`sys_ioctl` 优先走 inode 派发。** 之前 `sys_ioctl` 把 fd≤2 写死成 console,fd>2 直接 `-ENOTTY`,**没有任何 fd→设备的 ioctl 派发**。这一章先给 fd>2 加派发:走 `FDTable→File→Inode→ops->ioctl`(`sys_ioctl.cpp:76` 起,函数 `sys_ioctl` 在 67 行):

```cpp
// 任意装了 File 的 fd(含 0/1/2)都先走 inode ops 派发
auto r = file->inode->ops->ioctl(file->inode, request, arg);
if (!r.ok() && r.error() == NotImplemented) return -ENOTTY;  // 默认 ioctl = "不是 tty"
```

> 两处细节。其一,**File 优先,console 是兜底**。这一章最初只给 fd>2 加派发、保留「fd≤2 写死 console」;但后续 GUI shell 那条线(step75)发现:GUI shell 把 stdio 绑到 PTY slave 后,若 `sys_ioctl` 对 0/1/2 仍硬走 console,TCSETS 会打到 console 而非 PTY slave,PTY 就一直留着 ECHO,busybox 行编辑会和终端模拟器双重回显。于是 dispatch 改成「先看 FDTable 有没有装 File——有(哪怕是 0/1/2)就走该 inode 的 ops,没装才回退 console」。这一章的代码雏形是「fd≤2 console 零变」,但那是对 066 当时的论断;真正的「0/1/2 也能指向 PTY」要等 dup2 那条线(见 0XX 章)合上。其二,`NotImplemented → -ENOTTY`(不是 ENOSYS):`to_errno(NotImplemented)` 本会映射成 ENOSYS,但 ioctl 的语义里「这个 inode 不处理 ioctl」该返 ENOTTY(Linux:对普通文件做 tty ioctl 返 ENOTTY)。所以特判一下;PTY ops 自己返的真错误(EINVAL 等)仍走正常映射。

## DevFS PTY 节点:/dev/ptmx 克隆 + /dev/pts/N

两条缝铺好,把 PTY 拧进去,让一个进程能走 Linux PTY ABI:

```
fd = open("/dev/ptmx")        // 克隆:分配一对 PTY,返 master inode 的 fd
ioctl(fd, TIOCGPTN, &n)       // 拿 pty 号
slave = open("/dev/pts/<n>")  // 拿 slave inode 的 fd(shell 端)
```

两件事。

**`/dev/ptmx` 的克隆 open。** 普通文件 `open` 返回它自己;`/dev/ptmx` 不一样——`open` 它要**克隆出一对新 PTY**,返回 master 的 inode。这靠第二条新接缝 `InodeOps::open` virtual(`inode.hpp:126`,默认返回原 inode),`PtmxOps::open` override 成「分配一对、返 master」(`pty_device.cpp:390`):

```cpp
class PtmxOps : public InodeOps {
    ErrorOr<Inode*> open(Inode* /*self*/, uint64_t /*flags*/) override {
        auto idx = pty_alloc();        // 找空槽分配一对 PTY(wire_slot 在内部把 master/slave ops 接上)
        if (!idx.ok()) return idx.error();
        return pty_master_inode(*idx); // 返 master inode(用索引拿,不直接索引 g_slots)
    }
};
```

**`/dev/pts/N` 的动态查找。** DevFs 的固定节点表装不下「按需分配」的 slave(slave 号是 open ptmx 时才定的)。于是给 DevFs 加一个 `set_dynamic_lookup` 回调:固定表查不到时回调,`devfs_init` 装的 resolver 解析 `pts/<N>` → `pty_slave_inode(N)`。DevFs 自己不认 PTY(保持 host 可测,层化干净——PTY 装配只在 kernel-only 的 `devfs_init.cpp`)。

**槽位注册表,不是动态分配。** `PtySlot g_slots[kMaxPtys]`(`pty_device.cpp:63`),每槽 inline 拥有 `Pty + master_inode + slave_inode`;槽位上限 `kMaxPtys=8` 定义在头里(`pty_device.hpp:44`)。`pty_alloc()` 找首个空槽,`wire_slot()` 把两个 inode 的 `ops` 指向共享的 `g_master_ops`/`g_slave_ops`、`fs_private = &slot.pty`(ops 据此找回 Pty)。inode 号编码 pty 索引:`kPtyInoBase=0x1000`,`ino = base + index`,`TIOCGPTN` 返 `ino - base`(高基址避撞 DevFs 自己的节点号)。固定 8 槽 = 简单 + 确定性,动态扩展留后。

> 一个藏在 move 赋值里的坑:槽位复用时想 `pty = Pty{}` 重置,可这会让 echo sink 的 `echo_ctx_` 指向被销毁的临时——悬垂。所以用 `Pty::reset()` 原位清 master 环 + `slave_tty_ = TTY{}`(新行规范)+ **重新 `set_echo_sink(this)`** 把 echo 锚回当前对象。move 赋值会搬走内部指针的指向,原位 reset 才安全。

## 控制终端:TIOCSCTTY + /dev/tty

最后一块是「控制终端」语义。一个 session leader(setsid 之后)可以挂一个终端当自己的控制终端:往后它(及其进程组)的 Ctrl+C 往这个终端投、`/dev/tty` 指向这个终端。这靠 `TIOCSCTTY` ioctl(挂在 slave inode 上)设 `Task::controlling_tty`,把 PTY 跟 session 绑上(setsid 早留了 pgid/sid 那条缝,这会儿接上)。`/dev/tty` 是「当前控制终端」的每进程别名——也走 DevFS 的 dynamic lookup,resolver 返回当前 task 的 controlling_tty 对应的 inode。

这一块接通后,PTY 不只是个数据管子,而是有完整终端会话语义:session + 控制终端 + 前台组 + Ctrl+C 信号投递(062 console 那套信号路径,现在 PTY 也能走)。
