---
title: 04 · connect_path:立即建连,无内核握手
---

# connect_path:立即建连,无内核握手

`connect_path`(`unix_socket.cpp:75-126`)是 `AF_UNIX` 跟 TCP 差别最大的一步。TCP 的 `connect` 发 SYN、等 SYN-ACK、再回 ACK(069 讲过的三次握手),全在协议状态机里跑;`AF_UNIX` 没有协议握手——`connect_path` **直接**在内核里把连接建好:

```cpp
cinux::lib::ErrorOr<void> UnixSocket::connect_path(const char* path) {
    // ... 校验 path 非空 ...
    // 1. 在 registry 锁下查 server
    UnixSocket* server = nullptr;
    {
        auto r = UnixRegistry::instance().lookup(path);
        if (!r.ok()) {
            return r.error();  // NotFound -> ENOENT
        }
        server = *r;
    }
    // 2. 校验自己没占用
    {
        auto g = lock_.irq_guard();
        if (bound_ || listening_ || connected_) {
            return cinux::lib::Error::InvalidArgument;
        }
    }
    // 3. new 一个 server 端的 child,wire 双向 peer
    UnixSocket* child = new UnixSocket(type_);
    if (child == nullptr) {
        return cinux::lib::Error::OutOfMemory;
    }
    {
        auto g     = lock_.irq_guard();
        connected_ = true;
        peer_      = child;
    }
    {
        auto g            = child->lock_.irq_guard();
        child->connected_ = true;
        child->peer_      = this;
    }
    // 4. 把 child 塞进 server 的 accept 队列
    if (!server->enqueue_accept(child)) {
        // Not listening / queue full -> connection refused. Unwind the wiring.
        auto g            = lock_.irq_guard();
        connected_        = false;
        peer_             = nullptr;
        auto g2           = child->lock_.irq_guard();
        child->connected_ = false;
        child->peer_      = nullptr;
        delete child;
        return cinux::lib::Error::ConnectionRefused;
    }
    return {};
}
```

四步:**查 registry 找 server**(锁单独拿、放掉)→ **校验自己** → **new child + wire 双向 peer** → **`enqueue_accept` 塞进 server 队列**。第 3 步 wire peer 时,client 和 child **各自拿自己的锁**(`unix_socket.cpp:103-112`),从不同时拿两把——同 `bind_path` 的纪律。

第 4 步失败处理是 `unix_socket.cpp:114-123` 那段 unwind:`enqueue_accept` 返 false 表示 server 没 `listen` 或者队列满了(`unix_socket.cpp:140-152`),`connect_path` 要把第 3 步设的 `connected_`/`peer_` 全部回滚,再 `delete child`。这是手写的「事务回滚」——freestanding 内核没有 RAII 事务套件,失败路径靠手动 unwind,每一步都要对称。

## 立即建连意味着 send 可以先于 accept

这是 `AF_UNIX` 跟 TCP 在测试侧一个很实在的差别。`connect_path` 返回时,连接**已经建好**——client 的 `peer_` 指 child,child 的 `peer_` 指 client,child 已经在 server 的 accept 队列里。所以 client 立刻就能 `send`:

```cpp
TEST_ASSERT_TRUE(client.connect_path("/unix_echo").ok());
// client -> server: send BEFORE accept.  connect_path already created the
// accepted child + wired the pair as peers, so bytes buffer in the child's
// RX ring until accept() pulls it off.
auto sr = client.send(msg, sizeof(msg));  // test_socket.cpp:308
// ...
auto acc = server.accept(nullptr, nullptr);  // test_socket.cpp:313
```

测试注释 `test_socket.cpp:304-306` 把这件事点得很清楚。`send` 把字节拷进**对端**(child)的 RX 环,child 此刻还在 server 的 accept 队列里没被取走——没关系,4 KB 的环(`kRxSize = 4096`,`unix_socket.hpp:155`)先缓冲着,等 `accept` 把 child 取出来再 `recv`。

真 Linux 的 STREAM `connect` 是**阻塞到 server `accept`** 的(三次握手 + accept 才算建连完成),所以 send-before-accept 在 Linux 上不成立。Cinux 这里是 loopback 友好的简化——`connect_path` 立即建连、字节缓冲在 child 环里,让确定性单线程测试不必靠多线程 + 调度就能验证 round-trip。这条边界 `unix_socket.hpp:23-28` 文件头注释也认领了。**别把这个简化当成 Linux 行为**。

## enqueue_accept:塞队列 + 叫醒 accept'er

`enqueue_accept`(`unix_socket.cpp:140-152`)是 `connect_path` 调到 server 上的方法:

```cpp
bool UnixSocket::enqueue_accept(UnixSocket* child) {
    auto g = lock_.irq_guard();
    if (!listening_ || accept_count_ >= kAcceptMax) {
        return false;
    }
    accept_queue_[accept_tail_] = child;
    accept_tail_                = (accept_tail_ + 1) % kAcceptMax;
    ++accept_count_;
#ifndef CINUX_HOST_TEST
    wake_one(accept_waiters_);
#endif
    return true;
}
```

环形队列(`accept_queue_[kAcceptMax]` + head/tail/count 三件套,`unix_socket.hpp:171-172`)塞一条;如果此刻有任务阻塞在 `accept` 上(`accept_waiters_`),`wake_one` 叫醒它让它重试。`#ifndef CINUX_HOST_TEST` 把 `wake_one` 编译掉——host 单测没有真调度器,阻塞路径返 `WouldBlock`。
