---
title: 裸命令 Permission denied：stat 的返回结构错位
---

# 裸命令 Permission denied：stat 的返回结构错位

2026-10-18，对应用户所述 pack-23 §4f。musl 1.2.5 静态 busybox 1.38.0，
单核 QEMU KVM，AHCI/ext2。裸 `uname` 稳定报 Permission denied，
`/bin/uname` 与 `/bin/busybox uname` 正常，失败时没有 execve 调用。

## 根因与修复前现场

错误码 13 确实由 ash 自己选出，但触发条件是内核返回了错误布局的 stat。
原 SyscallStat 是五个 64 位字段，共 40 字节：ino、mode、links、size、mtime。
musl 按 Linux x86_64 的 144 字节结构读取；其定义可对照
[Linux UAPI](https://github.com/torvalds/linux/blob/master/arch/x86/include/uapi/asm/stat.h)
与本地 `/tmp/musl-1.2.5/arch/x86_64/{bits/stat.h,kstat.h}`。

| 字段 | 原内核偏移 | musl 读取偏移 |
| --- | ---: | ---: |
| ino | 0 | 8 |
| links | 16 | 16 |
| mode | 8（64 位） | 24（32 位） |
| size | 24 | 48 |
| mtime | 32 | 88 |

因此，musl 把文件大小 1214352（0x128790）的低 32 位当成 st_mode。
ash 的 `find_command()` 先 stat 每个 PATH 候选，成功后令 `e = EACCES`，
再调用 `test_exec()` 检查文件类型和执行位。这个假 mode 碰巧符合普通文件类型，
但 `0x128790 & 0x49 == 0`，于是直接拒绝，尚未进入 fork/exec。
普通执行中，带斜杠的命令跳过这一 PATH 候选检查，因此绝对路径可以成功。
`find_command()` 的 DO_ABS 查询模式另有 stat 检查，不应推广为所有含斜杠的查询都跳过。

在 busybox_unstripped 中，`test_exec` 地址为 0x4711f3，第一条指令正是
`mov 0x18(%rdi),%eax`。使用此二进制对应的 stripped ELF，在独立 KVM 的
硬件断点抓到：

```text
ASH_TEST_EXEC mode=0x128790 IX=0 size=288230513590665216
/bin/sh: line 0: uname: Permission denied
path-uname-status-127
absolute-uname-status-0
```

hello 的假 mode 则是其文件大小 0x3788，连普通文件类型检查也不满足。
错误路径上既没有未实现 syscall，也没有 execve，与用户观察一致。

## 为什么没有沿 /proc/self/exe 继续修

本地 `.config` 与生成的 `include/autoconf.h` 均关闭了
FEATURE_PREFER_APPLETS、FEATURE_SH_STANDALONE、FEATURE_SH_NOFORK。
`CONFIG_BUSYBOX_EXEC_PATH="/proc/self/exe"` 确实存在，但当前裸命令路径
先在 `find_command()` 的 stat 检查失败，尚未到达执行阶段。

宿主 strace 对同一 busybox_unstripped 和一个独立 PATH 目录得到：

```text
stat(".../bin/uname", {st_mode=S_IFREG|0755, st_size=1214352, ...}) = 0
execve(".../bin/uname", ["uname"], ...) = 0
```

这一执行链没有访问 `/proc/self/exe`。ash 在这里使用自己的 PATH 搜索和
`tryexec()` → execve，并不经过 musl execvp；应断 `test_exec()`、
`find_command()` 或 stat 返回，而非把 execvp 当作必经点。

## 修改

- SyscallStat 改为 Linux x86_64 的完整 144 字节布局，固定字段宽度，
  添加大小和关键偏移断言。内部 FsStat 继续保留其摘要形式。
- stat/lstat 和 fstat 清零完整返回记录后填写已知字段；newfstatat 经由
  HandleStat 复用同一布局。输出范围按完整 144 字节检查，拒绝空指针和越界。
- 修正教学用户 API 中 stat 输出参数的 const，重新编译教学 shell。
- 宿主测试以独立的系统 `struct stat` 核验大小、字段偏移和 mode 宽度；
  busybox 回归增加 PATH 裸 uname、裸 hello、缺失裸命令，以及绝对路径 uname。

四入口探针另外发现，openat/newfstatat 把 fd 参数按 64 位比较，误拒绝
零扩展的 `AT_FDCWD`（0x00000000ffffff9c）。现改为按 int32 解码；
宿主回归覆盖零扩展和符号扩展两种传参。此问题与裸命令的 EACCES 分别验证。

FsStat 尚未提供的设备号、访问/变更时间、块计数和首选 I/O 大小目前返回零；
uid/gid 对应当前单用户 root 模型。此次修复保证返回布局正确，不代表已经补全
所有 stat 元数据或 Linux 权限执行机制。旧的 40 字节教学 stat 调用者必须重编，
不能把旧教学 shell 二进制继续配给新内核。

## 验证与复跑

修复后，同一用户态断点记录：

```text
ASH_TEST_EXEC mode=0x81ed IX=0x49 size=1214352
path-uname-status-0
absolute-uname-status-0
path-hello-status-42
EXEC_SMOKE pid=2 code=0
```

- 52 个宿主测试目标通过，包含 stat ABI 编译断言和 AT_FDCWD 回归。
- KVM busybox 连续 20 次显式 fork/exec，以及裸命令、绝对路径、hello、
  false、缺失命令和父 ash 退出检查通过。
- 独立 musl 探针通过原始 syscall 分别调用 stat、lstat、fstat、newfstatat，
  检查 mode/size/ino/links/mtime、纳秒和保留字段、记录末尾 canary；
  空输出指针及跨用户地址上界的输出范围返回 EFAULT。

```sh
cmake --build build --target boot_image user_shell test_host -j 6
python3 scripts/test_execve_busybox.py \
  --busybox /tmp/busybox-1.38.0/busybox --rounds 20
```

脚本使用独立磁盘副本，创建 `/bin/uname` 后测试。原 ext2.img 没有被重建或
替换。若要在教学盘运行重编后的自家 shell，需将新的 build/user/shell.elf
更新到该盘；如果使用原 busybox 二进制，则无需重编 busybox。

现场文件：修复前 `/tmp/cinux-execve-vr4br2gr/`，修复后用户态断点
`/tmp/cinux-execve-559bit1c/`，四入口探针 `/tmp/cinux-execve-rhqjtqdh/`，
宿主 strace `/tmp/cinux-ash-host-h5fbblwf/strace.log`，最终 20 轮回归
`/tmp/cinux-execve-byubbwz3/`。
