---
title: 05 · `cinux_exit`:暴露给 ring3 的 QEMU 退出口(不是 `sys_exit`)
---

# `cinux_exit`:暴露给 ring3 的 QEMU 退出口(不是 `sys_exit`)

独立小节讲这个 Cinux 专有偏离。它和前面十四个号不一样——它不是 Linux ABI 拼图的一部分,是 Cinux 为了自动化验证**自己加**的一个号。

## 问题背景:用户态怎么让 QEMU 退出

buildroot 用户态(ash + 测试脚本)跑完测试要给 CI 一个 pass/fail 码——可用户态不能直接 `outb`(没 `iopl`/`ioperm`),`sys_reboot` 是 `-EPERM` 桩(busybox init 探它但不真做)。所以 `isa-debug-exit`(QEMU 的退出口设备,port 0xf4)这条路径只能靠一个**专有 syscall** 暴露给 ring3。

实现极其朴素(`sys_cinux_exit.cpp:27`):

```cpp
int64_t sys_cinux_exit(uint64_t code, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    // Never returns: QEMU exits with (code<<1)|1. Truncated to uint32_t to
    // match the device's iosize=4 (and the outl width the test harness uses).
    cinux::io::io_outl(kQemuExitPort, static_cast<uint32_t>(code));
    return 0;  // unreachable -- QEMU has exited
}
```

(`sys_cinux_exit.cpp:24-29`,真正写端口那行在 27。)

一条 `io_outl(0xf4, code)` 终止 QEMU,退出码是 `(code<<1)|1`(0→QEMU 退 1=`qemu_test_wrapper.sh` 映射 SUCCESS;非零→退 3+ = 失败)。它复用了测试框架(`kernel/test/main_test.cpp`)和 panic 路径(`exception_handlers.cpp`)在内核态做同一件事的原语,只是把它**暴露给 ring 3**。

## 头号混淆点:`cinux_exit` ≠ `sys_exit`/`sys_exit_group`

这是这一节必须讲清的——这俩名字像,量级完全不同。

- `sys_exit`(60)/`sys_exit_group`(231)是**进程卷**的事:把当前 task 标 `Zombie`、编码 waitpid status word、唤醒父进程 reap。**当前 ring3 程序**结束了,但内核和其他 task 还在跑。
- `sys_cinux_exit`(221)跟进程没关系——它 `outl` 端口让**整个 QEMU 进程**退出。不是退出程序,是退出**整台虚拟机**。

混淆了会把 CI gate 写成「程序退出就 PASS」,实际要的是「QEMU 退出码对」——前者任何一个 `exit(0)` 都触发,后者只有 `cinux_exit(0)` 才触发。头文件注释把这俩的区别点透(`sys_cinux_exit.hpp:5-14`):

```
Cinux-custom syscall (SYS_cinux_exit = 221). Lets a Buildroot userland
(ash + a test script) terminate the QEMU run with a pass/fail code that CI
gates on: ... Userspace cannot outb directly (no iopl/ioperm), and
sys_reboot is a -EPERM stub, so this syscall is the ONLY userspace ->
isa-debug-exit path.
```

## 占号问题:221 在 Linux 是 `fadvise64`,不是 fcntl

Linux x86_64 表里 221 是 `fadvise64`(`syscall_64.tbl` 里那行 `221 common fadvise64 sys_fadvise64`)——不是 fcntl。fcntl 在 x86_64 是 72(`syscall_nums.hpp:52` 也对:`SYS_fcntl = 72`)。Cinux 把 221 占为专有退出,实际占的是 Linux `fadvise64` 的号。

为什么不撞自己的 fcntl?因为 Cinux fcntl 走 `SYS_fcntl=72`(`syscall_nums.hpp:52`),musl/glibc 都走 wrapper 发 72 不发裸 221。这是**有意的、文档化的 ABI 偏离**——用户态触发器 `tools/musl/cinux-exit.c` 手动 `#define SYS_cinux_exit 221` 并注释「Keep in sync with kernel/syscall/syscall_nums.hpp」(`cinux-exit.c:19-21`),因为 musl 的 `sys/syscall.h` 没这个号:

```c
/* Cinux-custom syscall; not in musl's sys/syscall.h. Keep in sync with
 * SYS_cinux_exit in kernel/syscall/syscall_nums.hpp. */
#define SYS_cinux_exit 221
```

(`cinux-exit.c:19-21`。)

诚实边界:理论上若有 Linux 程序硬发裸 221 期望 `POSIX_FADV_*` 预读建议语义(那才是 Linux 221 的真身),会被 `cinux_exit` 误触发 QEMU 退出。**但 musl/glibc 的 `posix_fadvise` wrapper 也走自己的号路径,不会发裸 221**——所以实际不撞。这是个已知但未深究的边界。

## CI gate 链:busybox init → 测试脚本 → cinux-exit → QEMU 退出码

整条 CI gate 链是这样:

```
busybox init → inittab ::once → cinux-usability-test.sh → cinux-exit [code]
   → sys_cinux_exit(221) → io_outl(0xf4) → QEMU 退出 (code<<1)|1
```

`cmake/qemu.cmake:604-618` 把这条链立成 `run-buildroot-usability` target——`-device isa-debug-exit,iobase=0xf4,iosize=0x04` 把 QEMU 的退出设备挂上,CI 跑完看 QEMU 进程退出码。

诚实边界:**生产环境(run target,无 isa-debug-exit 设备)下这个 syscall 变 no-op**——`io_outl` 写一个没人听的端口,什么都不会发生。这是「Cinux 不是 Linux」的一处必要偏离——为了自动化验证必须有一个 Linux 没有的 syscall,但它只服务于 CI,不影响 POSIX 兼容(真 Linux 程序不会发 221 当 fadvise64,因为 musl/glibc wrapper 走自己的号路径)。
