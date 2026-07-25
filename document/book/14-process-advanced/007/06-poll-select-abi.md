---
title: 06 · poll vs select:两个 ABI 共享 poll_core
---

# poll vs select:两个 ABI 共享 poll_core

poll 和 select 在 Cinux 里是「**两套 ABI、一个核心**」。核心 `do_poll_core` 不知 poll/select 为何物——签名只接 `kpollfd*` + nfds + timeout_ms(`poll_core.hpp:51`),内部没有任何「poll 模式 / select 模式」分支。两个 wrapper 的差别纯粹是「**数据形态转换**」。

## sys_poll:直接搬 pollfd 数组

`sys_poll`(`sys_poll.cpp:31`)是薄薄一层用户态边界 wrapper:

```cpp
// kernel/syscall/sys_poll.cpp:31
int64_t sys_poll(uint64_t fds_virt, uint64_t nfds, uint64_t timeout, uint64_t, uint64_t, uint64_t) {
    if (nfds > kPollMaxFds) {  // kPollMaxFds=64,栈上 pollfd 上限
        return -cinux::kEinval;
    }
    if (nfds == 0) {
        // poll(NULL, 0, timeout):可移植亚秒睡眠,无数组要拷
        return do_poll_core(nullptr, 0, static_cast<int64_t>(timeout));
    }
    if (fds_virt == 0) {
        return -cinux::kEfault;
    }

    kpollfd kfds[kPollMaxFds];  // 栈上 512B
    if (!cinux::user::copy_from_user(kfds, reinterpret_cast<void*>(fds_virt),
                                     nfds * sizeof(kpollfd))) {
        return -cinux::kEfault;
    }

    int64_t ready = do_poll_core(kfds, nfds, static_cast<int64_t>(timeout));

    if (!cinux::user::copy_to_user(reinterpret_cast<void*>(fds_virt), kfds,
                                   nfds * sizeof(kpollfd))) {
        return -cinux::kEfault;
    }
    return ready;
}
```

用户给的就是 `pollfd` 结构数组,wrapper 直接 `copy_from_user` 整块拷进来(nfds×8B),跑完 `copy_to_user` 整块拷回去。`pollfd` 是「紧凑结构数组」语义——每个 fd 自带想看什么事件(events)、就绪什么事件(revents)。栈上 cap `kPollMaxFds=64`(`sys_poll.cpp:29`)够真实 app 用(sh poll stdin、nc/wget poll 一个 socket),再大就 `-EINVAL` 而不是 heap alloc——hobby-OS 用「上限即真理」省栈空间。`poll(NULL, 0, timeout)` 是可移植亚秒睡眠的特殊用法(无数组要拷),`do_poll_core(nullptr, 0, timeout)` 直走核心。

## sys_select:fd_set 位图 ↔ pollfd 翻译

`sys_select`(`sys_select.cpp:69-191`)用户给的是 3 张位图(`fd_set`:readfds/writefds/exceptfds)+ nfds。wrapper 先扫描 fd_set 把被置位的 fd 合并成 pollfd 数组,交给 `do_poll_core`;跑完按返回的 revents 把就绪 fd 回填进 3 张输出位图。

**fd_set → pollfd 翻译**(`sys_select.cpp:114-137`):

```cpp
// kernel/syscall/sys_select.cpp:114
kpollfd  pfds[kPollMaxFds];
uint64_t count = 0;
for (uint64_t fd = 0; fd < nfds; ++fd) {
    uint16_t ev = 0;
    if (readfds != 0 && fd_is_set(rd, static_cast<int>(fd))) {
        ev |= cinux::fs::kPollIn;    // read 位置位 -> POLLIN
    }
    if (writefds != 0 && fd_is_set(wr, static_cast<int>(fd))) {
        ev |= cinux::fs::kPollOut;   // write 位置位 -> POLLOUT
    }
    if (exceptfds != 0 && fd_is_set(ex, static_cast<int>(fd))) {
        ev |= cinux::fs::kPollPri;   // except 位置位 -> POLLPRI
    }
    if (ev == 0) {
        continue;  // 这个 fd 三个集合都没置位 -> 跳过
    }
    if (count >= kPollMaxFds) {
        return -cinux::kEinval;  // too many watched fds for the stack cap
    }
    pfds[count].fd      = static_cast<int32_t>(fd);
    pfds[count].events  = static_cast<int16_t>(ev);
    pfds[count].revents = 0;
    ++count;
}

int64_t ready = do_poll_core(pfds, count, timeout_ms);
```

稀疏集合只生成被 watch 的 fd 项,不浪费 pollfd 槽。

**revents → fd_set 回填**(`sys_select.cpp:144-165`):

```cpp
// kernel/syscall/sys_select.cpp:144
// Rebuild the output sets IN PLACE (zero, then set ready bits).  POLLHUP/
// POLLERR on a watched read fd surface in the read set so the app wakes and
// reads EOF / gets the error (Linux reports them this way).
for (uint64_t i = 0; i < kSetBytes; ++i) {
    rd[i] = 0; wr[i] = 0; ex[i] = 0;  // 先清零输出集合
}
for (uint64_t i = 0; i < count; ++i) {
    uint16_t rv = static_cast<uint16_t>(pfds[i].revents);
    int      fd = pfds[i].fd;
    if (rv == 0) {
        continue;
    }
    if (readfds != 0 &&
        (rv & (cinux::fs::kPollIn | cinux::fs::kPollHup | cinux::fs::kPollErr))) {
        fd_set_bit(rd, fd);  // POLLIN/HUP/ERR 都进 read set(Linux 反直觉语义)
    }
    if (writefds != 0 && (rv & cinux::fs::kPollOut)) {
        fd_set_bit(wr, fd);
    }
    if (exceptfds != 0 && (rv & cinux::fs::kPollPri)) {
        fd_set_bit(ex, fd);
    }
}
```

注意一个反直觉但符合 Linux 的取巧——**POLLHUP/POLLERR 也进 read set**(`sys_select.cpp:155-157`)。一个写端关闭的 pipe,poll 在读 fd 报 POLLHUP,select 会把这个 fd 在 readfds 置位,让 app 醒来 read 拿 EOF 或错误,而不是永远阻塞。这条语义的依据是 Linux man select(2):「a file descriptor that has reached end-of-file will be reported as ready for reading」。

## 两个有意思的取巧

**取巧一:只拷 fd_set 低 32B。** `kSetBytes = FD_TABLE_SIZE / 8 = 32B`(`sys_select.cpp:41`),即 256 位。用户态 `fd_set` 是 `FD_SETSIZE=1024` 位 = 128B,但 Cinux 一个进程最多持 `FD_TABLE_SIZE=256` 个 fd(`file.hpp`),bits 256..1023 永远不可能就绪,所以只拷 32B,nfds 上界 clamp 到 256(`sys_select.cpp:72-74`)。这是 hobby-OS 用「上限即真理」省栈空间的典型——栈上工作集从 384B(三张 128B 位图)压到 96B(三张 32B)。

**取巧二:*timeout 回填剩余时间。** Linux 的 select 会把 `*timeout` 更新为「剩余时间」(从不增加),Cinux 照搬(`sys_select.cpp:180-188`):

```cpp
// kernel/syscall/sys_select.cpp:180
if (has_deadline) {
    uint64_t now    = monotonic_ns();
    int64_t  rem_ns = (now >= deadline) ? 0 : static_cast<int64_t>(deadline - now);
    ktimeval tv;
    tv.tv_sec  = rem_ns / static_cast<int64_t>(kNsPerSec);
    tv.tv_usec = (rem_ns % static_cast<int64_t>(kNsPerSec)) / 1000;
    static_cast<void>(cinux::user::copy_to_user(reinterpret_cast<void*>(timeout_virt), &tv, sizeof(tv)));
}
```

这条语义让 app 能用 select 实现「在 N 个 fd 上分时片轮转」——每次 poll 后看剩余时间决定下次等多久。

## SYS_poll=7 是「换芯」而非新增

`SYS_poll = 7`(`syscall_nums.hpp:31`)很早就占了号——`syscall_nums.hpp:31` 的注释明说 `(stub: busybox sh)`,当年那是个 stub,让 busybox sh 能起来。后来才把 handler 换成真阻塞 `do_poll_core`。所以 `syscall.cpp:170` 的源码注释是 `F8-M5 real poll` 而非 `add poll`。

对比 `SYS_select = 23`(`syscall_nums.hpp:44`,注释 `select (F8-M5 real poll/select)`)是同时新加的号。注册在 `syscall.cpp:170-171`:

```cpp
// kernel/arch/x86_64/syscall.cpp:170
syscall_register(SyscallNr::SYS_poll, sys_poll);            // F8-M5 real poll
syscall_register(SyscallNr::SYS_select, sys_select);        // F8-M5 real select
```

把「号早就占了、内核却很晚才会真等」的演进讲清楚,否则学习者会困惑「为什么 poll 是低号(7)但实现晚」——答案是早期那个号是个 stub(具体返回什么已不可考,工作树里只有换成真阻塞之后的实现),后来才把芯换成真阻塞 `do_poll_core`。这跟 SYS_select 这种「号和实现同时落地」的路径不一样。
