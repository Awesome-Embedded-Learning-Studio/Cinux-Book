---
title: 06 · close、copy_from_user 陷阱与阻塞模板
---

# close、copy_from_user 陷阱与阻塞模板

## close:撤名 + 叫醒所有等待者

`close`(`unix_socket.cpp:295-328`)做三件事:

```cpp
void UnixSocket::close() {
    UnixSocket* peer = nullptr;
    {
        auto g = lock_.irq_guard();
        if (closed_) {
            return;  // idempotent
        }
        closed_ = true;
        peer    = peer_;  // snapshot (write-once)
    }
    // 1. 通知对端:它的 send 该 EPIPE 了,它的 recv 该 EOF 了
    if (peer != nullptr) {
        auto g          = peer->lock_.irq_guard();
        peer->peer_eof_ = true;
#ifndef CINUX_HOST_TEST
        wake_all(peer->recv_waiters_);  // blocked recv'ers retry -> drained ring -> EOF
#endif
    }
    bool was_listening = false;
    {
        auto g        = lock_.irq_guard();
        was_listening = listening_;
        listening_    = false;
#ifndef CINUX_HOST_TEST
        wake_all(recv_waiters_);       // unix_socket.cpp:319
        wake_all(accept_waiters_);     // unix_socket.cpp:320
#endif
    }
    // 2. 监听 socket 释放名字,让后续 bind 能复用
    if (was_listening && bound_) {
        UnixRegistry::instance().unregister(path_);  // unix_socket.cpp:325
        bound_ = false;
    }
}
```

三步:**通知对端 EOF + 叫醒对端的 recv 等待者**(`unix_socket.cpp:306-311`)→ **叫醒自己两侧的等待者**(`recv_waiters_` 和 `accept_waiters_`,分别在 `unix_socket.cpp:319` 和 `:320`)→ **从 registry 撤名**(只对监听 socket,`unix_socket.cpp:324-326`)。

第二步的两处 `wake_all` 不要合成一个区间——它们叫醒的是**两个不同的队列**:L319 叫醒 `recv_waiters_`(阻塞在 recv 上的任务),L320 叫醒 `accept_waiters_`(阻塞在 accept 上的任务)。被叫醒的任务重试后会发现环空 + `peer_eof_` / 队列空 + `closed_`,各自走对应的错误路径(recv 返 0 EOF,accept 返 `InvalidArgument`)。这就是「lost-wakeup」防卫——`close` 必须把所有可能睡在这个 socket 上的任务都摇醒,否则它们会睡死。

### release 钩子:close 怎么被触发

到这里要澄清一个容易踩的坑。早期 dev note 里写过一句「close 无 `InodeOps::release` 钩子,`sys_close` 只 `free File`,不调 `Socket::close()` → listener 不从 registry 撤名」——**这条对当前 Book 源码已经过期**。release 钩子在 `SocketOps` 上已经补上了(`socket.hpp:196-198` 声明、`socket.cpp:148-156` 实现):

```cpp
// socket.hpp:196-198
// F8-M5 release: closing a socket fd releases its protocol resources
// (unbind / stop_listen / FIN).  One Socket per fd, so no refcounting needed.
void     release(cinux::fs::Inode* inode) override;

// socket.cpp:148-156
void SocketOps::release(cinux::fs::Inode* inode) {
    auto* s = socket_of(inode);
    if (s != nullptr) {
        s->close();
    }
}
```

所以 `sys_close` 关一个 socket fd 时,fd 层走 `InodeOps::release` → `SocketOps::release` → `Socket::close()` → `UnixSocket::close()` → `UnixRegistry::unregister(path_)`——**listener 关闭即让出 path 名字**,后续 `bind` 同名能成功。这条链路是通的,本章验证节会跑一次「close 后重 bind 同名」作为端到端证据。早期那条 dev note 的限制描述的是补 release 钩子之前的旧状态,引用时要注明该限制已修复——别照搬旧措辞说「close 不撤名」。

## copy_from_user 的陷阱:为什么 echo 测试不走 sys_bind

`test_socket.cpp:265-275` 那段块注释把这件事讲得很透:

> `AF_UNIX` has no NIC / L4 module / loopback device: `connect_path()` wires two sockets as peers through an in-memory name registry (`UnixRegistry`), and `send()` copies bytes straight into the peer's RX ring. `test_unix_socket_returns_fd` drives the SYSCALL creation path (`sys_socket(AF_UNIX)` -> `UnixSocket` behind a `SocketOps` fd). The echo + negatives drive the UnixSocket METHODS directly -- the same choice the AF_INET TCP/UDP echo tests make, because the ring0 test kernel cannot hand `sys_bind` a "user" address (`copy_from_user`'s `is_user_vaddr` range check rejects kernel-stack pointers, so the `sockaddr_un` parsing in `sys_bind` is exercised in production via musl, covered here by inspection).

根因在 `copy_from_user` 的范围检查(`user_access.hpp:66-79` 的 `access_ok` + `paging_config.hpp:60-62` 的 `is_user_vaddr`):

```cpp
// paging_config.hpp:60-62
constexpr bool is_user_vaddr(uint64_t virt) {
    return !(virt & (1ULL << 47));
}

// user_access.hpp:66-79
inline bool access_ok(const void* addr, size_t size) {
    uint64_t a = reinterpret_cast<uint64_t>(addr);
    if (a == 0) { return false; }
    uint64_t last = (size == 0) ? a : end - 1;
    return cinux::arch::is_user_vaddr(a) && cinux::arch::is_user_vaddr(last);
}
```

`is_user_vaddr` 的判据是 **bit 47 = 0**(用户态 canonical 下半区,`0x0000...` 到 `0x7FFF...`)。ring0 测试内核跑在内核高半区,栈指针 bit 47 = 1,所以 `access_ok` 直接拒——`copy_from_user` 返 false,`parse_sockaddr_un` 跟着返 false,`sys_bind` 映射成 `-EFAULT`(`sys_socket.cpp:244-246`)。测试想给 `sys_bind` 喂一个栈上的 `sockaddr_un`,根本过不了这一关。

所以 echo 测试的策略是:**走 `UnixSocket` 的直接方法**(`bind_path`/`connect_path`/`send`/`recv`),绕开 `sys_bind` 这层用户边界。这不是 `UnixSocket` 的 bug——`is_user_vaddr` 这个范围检查是 SMAP/extable 安全模型的一部分(防止内核被诱导读写用户态范围外的地址),生产路径上 musl 给的是真用户地址,过得了 `access_ok`。测试侧的这条妥协,跟 063 UDP / 069 TCP 的 echo 测试是同一个选择(那两章注释也明说 ring0 测试内核喂不了用户地址)。

只有 `test_unix_socket_returns_fd` 走 `sys_socket`(`test_socket.cpp:276-292`)——因为 `sys_socket` 不吃用户指针(参数是 `domain`/`type`/`protocol` 三个整数),不触发 `copy_from_user`,所以这条路径在 ring0 测试内核里能跑。它验证的是「`socket(AF_UNIX, SOCK_STREAM, 0)` 真能造出一个 `SocketOps` fd」这条 syscall 链路。

## 阻塞:抄 prepare_to_wait,不 sti/hlt

`recv` 空环、`accept` 队列空,真睡的代码长这样(`unix_socket.cpp:268-280` 的 recv 段、`:174-187` 的 accept 段):

```cpp
// recv 空环 + 对端没 close
Task* self = Scheduler::current();
if (self == nullptr) {
    return cinux::lib::Error::WouldBlock;
}
wait_enqueue(recv_waiters_, self);
Scheduler::prepare_to_wait(self);
need_block = true;
// ... 出锁 ...
if (need_block) {
    Scheduler::schedule_blocked();
}
```

`prepare_to_wait` + `schedule_blocked` 这一对是 Cinux 真调度等待队列的标准模板。`recv_waiters_`/`accept_waiters_` 是侵入式链表头(`unix_socket.hpp:180-181`),`wait_enqueue`/`wake_one`/`wake_all` 是 `wait_queue.hpp` 提供的共享原语(`unix_socket.cpp:20` include,这玩意儿原来在 tcp/udp/unix 三处重复,后来抽出来共享)。

这套模板的来历在 071 章——那里把 pipe 阻塞从 `sti`/`hlt` 自旋改成 `prepare_to_wait` + `schedule_blocked`,根因是 `sti`-in-syscall 的 `#DF` 隐患(059 sys_ping 首发现同一族):syscall 里 `sti` → LAPIC 时钟中断抢 `%gs:0` 栈陷阱帧 → sysretq 弹花 → `#DF`,而且 harness 的「假绿」盖着这条坑。071 给的修法就是真调度等待队列,`AF_UNIX` 这里**直接复用**同一个模板——`unix_socket.cpp:5-9` + `:20-21` 的注释明说 mirrors `pipe.cpp`/`tcp_socket.cpp`、NO `sti`/`hlt`。

EINTR 的处理也跟 071 同款:`recv`/`accept` 睡回来后检查 `signal_deliverable_pending`,若有信号挂起,返一个 sentinel(`recv` 返 `-1`、`accept` 返 `-1`-cast-to-`Socket*`),由 `sys_recvfrom`/`do_accept` 映射成 `-EINTR`(`sys_socket.cpp:336-338` / `:188-190`)。这两个 sentinel 是一个真实字节数/指针取不到的值,用来在 `ErrorOr` 不能扩展 `lib::Error`(那是 Cinux-Base 子模块)的前提下把 EINTR 传过协议层。

### host 单测把阻塞编译掉

`#ifndef CINUX_HOST_TEST` 这套守卫在 `unix_socket.cpp:24-27`:

```cpp
#ifndef CINUX_HOST_TEST
#    include "kernel/proc/process.hpp"  // Task + signal_deliverable_pending
#    include "kernel/proc/scheduler.hpp"  // prepare_to_wait/schedule_blocked/unblock
#endif
```

host 单测没有真调度器(`Scheduler::current()` 返 `nullptr`),所以阻塞路径返 `WouldBlock`,而 `wake_one`/`wake_all`/`prepare_to_wait`/`schedule_blocked` 全部编译掉。这让 `UnixSocket` 能在 host 上做纯非阻塞的逻辑测试(`recv` 空环返 `WouldBlock`、环有数据立刻取走),target 上才有真阻塞。
