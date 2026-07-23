---
title: Lab 073 · busybox 用户态生态 + socket API 验证
---

# Lab 073 · busybox 用户态生态 + socket API 验证

> 对应 `document/book/07-userland/073-busybox-socket-ecosystem.md`。验证档 **A 档**:punchline 是 busybox 14 applet 真跑出真输出(真二进制,不是机制测)+ socket loopback echo。busybox 端到端要 build-busybox.sh 编 busybox + ext2 装盘 + QEMU fork+execve;socket 靠内核 loopback echo 测。

## 目标

确认六件事:

1. **busybox 真跑通**(14 applet 真输出,用户态试金石);
2. **机制测 ≠ 真二进制**:机制测绿不等于 busybox 能跑(缺环境件);
3. **socket ABI 全套**(socket/bind/connect/listen/accept/sendto/recvfrom,Socket=InodeOps 子类);
4. **loopback UDP+TCP echo** 干通;
5. **getdents64 + chmod/chown/link/rename/utimensat/dup/fcntl/nanosleep/sysinfo** 等 syscall 补全;
6. **kernel/fs 分子目录**(ext2//procfs//devfs//ramdisk/)。

## 步骤

### 1. busybox 14 applet 真跑(需 build-busybox)

要 build-musl + busybox:

```bash
tools/musl/build-musl.sh
tools/musl/build-busybox.sh        # busybox 1.36 static musl,用 busybox-1.36.0.config
```

然后 ext2 装 busybox + 环境件(create_ext2_disk.sh 自动),QEMU 跑:

```bash
cmake --build build --target run
```

进 shell 后(busybox 装在 /bin),挨个试 14 applet:`/bin/busybox echo hi` / `/bin/busybox id`(出 `uid=0(root)`)/ `/bin/busybox whoami`(`root`)/ `/bin/busybox pwd`(`/`)/ `/bin/busybox ls` / `/bin/busybox ps` / `/bin/busybox free`(`Mem: 9437184 ...`)/ `/bin/busybox uname` / `/bin/busybox cat /hello` / `/bin/busybox true; echo $?`(0)/ `/bin/busybox false; echo $?`(1)。真输出 + 对的退出码 = 真跑通。

> 没编 busybox 的话,这道跳过,靠步骤 3(socket)+ 步骤 5(syscall 机制测)+ grep 锚点验内核侧。

### 2. 环境件(/proc/meminfo + /etc/passwd)

busybox 真跑要几个环境件(非内核 bug,是盘/环境缺):

```bash
grep -nE "meminfo|MemTotal|MemFree" kernel/fs/procfs/procfs.cpp kernel/fs/procfs/*.cpp 2>/dev/null | head
grep -nE "passwd|group" scripts/create_ext2_disk.sh | head
```

应看到 `/proc/meminfo` 伪文件(ProcFS 新加,从 g_pmm 生成 MemTotal/Free,give `free` grep)+ create_ext2_disk.sh 加 `/etc/passwd` + `/etc/group`(give whoami/id 解析 uid 0→root)。这是「真二进制验收」挖出来的:机制测绿,可 busybox 真跑才发现缺这些。

### 3. socket ABI(loopback echo)

```bash
cmake --build build --target run-kernel-test 2>&1 | grep -iE "socket" | head
```

应看到 `test_socket` 一批 PASS:出 fd / 参数校验 / fd→Socket 路由 / **UDP echo**(client→server→client round-trip)/ **TCP echo**(connect→握手→accept→双向 echo)/ close。

### 4. Socket = InodeOps 子类

```bash
grep -rnE "class SocketOps|class UdpSocket|class TcpSocket|: public InodeOps" kernel/net/ 2>/dev/null | head
ls kernel/net/*socket* kernel/net/socket* 2>/dev/null
```

应看到 `SocketOps : InodeOps`(对齐 PTY/pipe) + UdpSocket/TcpSocket 适配器。socket fd 就是 pipe fd(fd→File→Inode→SocketOps),sys_read/write/close 零改动。协议层(UdpModule/TcpModule)保持纯,per-socket RX 环 + accept 队列放适配器,挂 listener 缝(UdpListener::on_udp / TcpListener::on_accept/on_data),回调拷贝帧进环(设备 buffer dispatch 后回收)。

### 5. syscall 补全(getdents64 + vfs + 杂)

```bash
ls kernel/syscall/sys_getdents64* kernel/syscall/sys_chmod* kernel/syscall/sys_link* kernel/syscall/sys_rename* kernel/syscall/sys_dup* kernel/syscall/sys_fcntl* kernel/syscall/sys_nanosleep* kernel/syscall/sys_sysinfo* 2>/dev/null | head -20
```

应看到这一批新 syscall(给 busybox 用):getdents64(ls)、chmod/chown/utimensat(属性)、link/rename/symlink(链接)、dup/fcntl(fd 控制)、nanosleep(sleep)、sysinfo(getrusage/free)。每个都有机制测(绿),但 busybox 真跑才是终验。

syscall 号(socket):socket=41/connect=42/accept=43/sendto=44/recvfrom=45/bind=49/listen=50(Linux x86_64 标准,40-55 空):

```bash
grep -nE "kSocket|kConnect|kAccept|kSendto|kRecvfrom|kBind|kListen" kernel/syscall/syscall_nums.hpp | head
```

### 6. kernel/fs 分子目录

```bash
ls kernel/fs/ext2/ kernel/fs/procfs/ kernel/fs/devfs/ kernel/fs/ramdisk/ 2>/dev/null | head -30
ls kernel/fs/*.hpp kernel/fs/*.cpp 2>/dev/null | head    # 核心 VFS 留根
```

应看到 4 个后端各进子目录(ext2/ 8 文件、procfs/ 5、devfs/ 3、ramdisk/ 3),核心 VFS(inode/file/vfs_mount/stat/path)留 kernel/fs/ 根。纯结构整理,零功能变。

两腿汇总:

```bash
cmake --build build --target run-kernel-test-all 2>&1 | grep -E "Tests: [0-9]{4} passed" | tail -1
```

应看到单核 + `-smp 2` 两腿各 `1061 passed, 0 failed`(1020 基线 + 41 用户态生态/socket 测),check_net_decoupling 绿。

## 验收清单

- [ ] (编了 busybox)14 applet 真跑通(echo/id/whoami/pwd/ls/ps/free/...真输出 + 对退出码)。
- [ ] `/proc/meminfo`(ProcFS,give free)+ `/etc/passwd`/`/etc/group`(give whoami/id)环境件在。
- [ ] `test_socket` PASS:UDP echo(client→server→client)+ TCP echo(connect→握手→accept→双向)。
- [ ] `SocketOps : InodeOps`(对齐 PTY/pipe),socket fd = pipe fd;UdpSocket/TcpSocket 适配器挂 listener 缝拷贝帧进环。
- [ ] getdents64/chmod/chown/link/rename/utimensat/dup/fcntl/nanosleep/sysinfo syscall 在;socket 号 41-50 对齐 Linux。
- [ ] kernel/fs 分子目录(ext2//procfs//devfs//ramdisk/ + 核心 VFS 留根);两腿 1061/0。

## 别做这些

- **别**以为机制测绿 = busybox 能跑——每批 syscall 机制测都绿,可 busybox 真跑才发现缺 `/proc/meminfo`、`/etc/passwd`(busybox 自己错退,非内核 bug)。真二进制验收(fork+execve busybox 看串口输出 + 退出码)不可替代。
- **别**给 File 加类型 tag 区分 socket/pipe——socket fd 就当 pipe fd 走(fd→File→Inode→SocketOps),sys_read/write/close 零改动。加 tag 要改每个 fd 消费者 + 未来 dup2/poll,是 Linux struct file 单缝避免的反模式。
- **别**在协议层(TcpModule/UdpModule)塞 per-socket 状态——协议层保持纯,per-socket RX 环/阻塞/accept 队列放 Socket 适配器,挂 listener 缝。回调里拷贝帧(设备 buffer dispatch 后回收,别存指针)。
- **别**用 sti/hlt 做 socket 阻塞——syscall 上下文 sti → #DF(059/071 同根)。用 prepare_to_wait/schedule_blocked,由 net_poll kthread 唤醒。
- **别**指望 socket TCP 可靠——仍最小可用(无重传/RTO/窗口/拥塞,069 范围)。loopback 零丢包 echo 稳,SLIRP 真网可能丢。要 HPET 周期中断。
- **别**以为 socket close 彻底——无 InodeOps::release 钩子(close 不彻底拆 Socket,同 pipe hobby 限制)。musl socket demo、setsockopt 精化、AF_UNIX、epoll 都留 follow-up。
