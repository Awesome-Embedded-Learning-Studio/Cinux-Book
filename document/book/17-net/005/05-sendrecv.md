---
title: 05 · send 与 recv:拷进对端环,排空自己环
---

# send 与 recv:拷进对端环,排空自己环

## send:拷进对端的 RX 环

`send`(`unix_socket.cpp:201-240`)的核心是把字节拷进**对端**的 RX 环,不是自己的:

```cpp
cinux::lib::ErrorOr<int64_t> UnixSocket::send(const uint8_t* buf, uint32_t len) {
    // ... buf 非空、shut_write 检查 ...
    UnixSocket* peer = nullptr;
    {
        auto g = lock_.irq_guard();
        if (!connected_ || closed_) {
            return cinux::lib::Error::InvalidArgument;  // ENOTCONN-shaped
        }
        peer = peer_;  // snapshot (write-once after connect/accept)
    }
    if (peer == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    // Copy into the PEER's RX ring under the peer's lock ONLY (no nested lock).
    uint32_t want = len > kRxSize ? kRxSize : len;
    auto     g    = peer->lock_.irq_guard();
    if (peer->closed_) {
        return cinux::lib::Error::BrokenPipe;  // peer closed -> EPIPE / SIGPIPE-shaped
    }
    uint32_t space = kRxSize - static_cast<uint32_t>(peer->rx_.size());
    if (space == 0) {
        // Ring full -> EAGAIN (send-side flow control 是 follow-up)
        return cinux::lib::Error::WouldBlock;
    }
    uint32_t n = want < space ? want : space;
    peer->rx_.push_batch(buf, n);
#ifndef CINUX_HOST_TEST
    wake_one(peer->recv_waiters_);
#endif
    return static_cast<int64_t>(n);
}
```

关键两步:**先在自己锁下 snapshot `peer_`**(write-once,connect/accept 之后不动,所以 snapshot 后可以放掉自己的锁)→ **只拿对端那一把锁**把字节 push 进 `peer->rx_`。这个加锁策略是「不嵌套两把 socket 锁」纪律的体现——`unix_socket.hpp:34-36` 文件头注释明说:peer_ 是 write-once,snapshot 后用,从不同时持两把 socket 锁,因此不可能 AB-BA。

`send` 末尾 `wake_one(peer->recv_waiters_)`(`unix_socket.cpp:236-238`)——如果对端正阻塞在 `recv` 上,叫醒它重试。

## recv:排空自己的环

`recv`(`unix_socket.cpp:242-293`)排空**自己**的环:

```cpp
cinux::lib::ErrorOr<int64_t> UnixSocket::recv(uint8_t* buf, uint32_t len, Ipv4Addr* /*out_src*/,
                                              uint16_t* /*out_port*/) {
    // ... buf 非空、shut_read 检查 ...
    for (;;) {
        {
            auto g = lock_.irq_guard();
            if (rx_.size() > 0) {
                uint32_t want = len < rx_.size() ? len : static_cast<uint32_t>(rx_.size());
                uint32_t got  = static_cast<uint32_t>(rx_.pop_batch(buf, want));
                return static_cast<int64_t>(got);
            }
            if (peer_eof_) {
                return static_cast<int64_t>(0);  // EOF: peer closed + ring drained
            }
            // ... host: WouldBlock; target: prepare_to_wait + schedule_blocked ...
        }
        // ... schedule_blocked + EINTR sentinel 处理 ...
    }
}
```

环里有数据就 `pop_batch` 取走;环空且 `peer_eof_`(对端 close 了)返 0(EOF 语义);环空且对端没 close,**真睡**。EINTR sentinel(`recv` 返 `-1` 这个真实字节数取不到的值)的处理在 `unix_socket.cpp:285-290`,由 `sys_recvfrom` 映射成 `-EINTR`(`sys_socket.cpp:336-338`)。

## send 侧流控是 follow-up

注意 `send` 在环满时返 `WouldBlock`(`unix_socket.cpp:227-233`),不是阻塞等对端 drain:

```cpp
uint32_t space = kRxSize - static_cast<uint32_t>(peer->rx_.size());
if (space == 0) {
    // Ring full: a blocking send would park on a write-wait queue, but the
    // single-thread test never fills a 4 KB ring with a tiny message.  True
    // send-side flow control (block until the peer drains) is a follow-up;
    // for now report EAGAIN so a looping caller backpressures itself.
    return cinux::lib::Error::WouldBlock;
}
```

真 Linux 的 socket `send` 在对端没 drain 时会阻塞(或返 `EAGAIN` 若非阻塞)。Cinux 这会儿**没有 send 侧的等待队列**——`recv` 侧有 `recv_waiters_`,但 `send` 侧没对应的 write-waiters。返 `EAGAIN`(映射成 `WouldBlock`)让调用方自己 loop 退避,是诚实的最小实现。测试用 4 字节消息填不满 4 KB 环,所以这条路径在 echo 测试里走不到;留 follow-up。
