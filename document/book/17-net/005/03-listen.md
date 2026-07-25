---
title: 03 · 两角色一类:UnixSocket 的 listening 与 connected
---

# 两角色一类:UnixSocket 的 listening 与 connected

## 两角色一类

`UnixSocket` 一个类同时扮演两个角色,靠几个 bool 区分(`unix_socket.hpp:158-163`):

```cpp
char path_[kUnixPathMax]{};  ///< bound leaf name (listener; for unregister)
bool bound_     = false;     ///< bind_path succeeded
bool listening_ = false;     ///< listen() succeeded
bool connected_ = false;     ///< connected (client after connect / accepted child)
bool closed_    = false;     ///< this end called close()
bool peer_eof_  = false;     ///< the peer called close() -> recv EOF once drained
```

- **监听端(server)**:`bind_path` 成功 → `bound_ = true`;`listen` 成功 → `listening_ = true`。`accept` 从 `accept_queue_` 取已连接的 child。
- **已连接端(client connect 之后、或 accept 出来的 child)**:`connect_path`/`pair_with`/accept wiring 把 `connected_ = true`、`peer_` 指向对端。`send` 拷进**对端**的 RX 环,`recv` 排空**自己**的 RX 环。

这跟 069 的 `TcpSocket` 镜像是同一套思路——一个类管两种状态,角色靠 bool 区分,而不是为「监听 socket」和「已连接 socket」各写一个类。好处是 `accept` 出来的 child 和 client 端用**同一个类型**(`UnixSocket`),只是 `peer_` 指向不同;`send`/`recv` 一份代码两种角色共用。

## bind_path:登记 + 留名

`bind_path`(`unix_socket.cpp:47-73`)做两件事:先在 socket 自己的锁下校验状态(不能已 bound/listening/connected),再**单独**拿 registry 锁登记,最后回 socket 锁复制 path 留底(给后面 `close` 反注册用):

```cpp
cinux::lib::ErrorOr<void> UnixSocket::bind_path(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return cinux::lib::Error::InvalidArgument;
    }
    {
        auto g = lock_.irq_guard();
        if (bound_ || listening_ || connected_) {
            return cinux::lib::Error::InvalidArgument;
        }
    }
    // Register under the REGISTRY lock only (no socket lock held) -> no AB-BA
    // with connect_path, which takes registry-then-socket.
    auto r = UnixRegistry::instance().register_listener(path, this);
    if (!r.ok()) {
        return r.error();  // AlreadyExists / OutOfMemory
    }
    auto     g = lock_.irq_guard();
    uint32_t i = 0;
    while (i + 1 < kUnixPathMax && path[i] != '\0') {
        path_[i] = path[i];
        ++i;
    }
    path_[i] = '\0';
    bound_   = true;
    return {};
}
```

注意那个注释「Register under the REGISTRY lock only (no socket lock held) -> no AB-BA」。这是加锁顺序纪律:`bind_path` 拿 registry 锁时**不持有** socket 锁;`connect_path`(下面讲)也是先拿 registry 锁查、放掉,再拿 socket 锁。两边都不在持 socket 锁时去碰 registry,所以 `bind_path` 和 `connect_path` 之间、以及它们和任何持 socket 锁的操作之间,都不可能形成 AB-BA 死锁。这条纪律 `unix_socket.hpp:30-36` 文件头注释写得很清楚。

`path_` 留底是为了 `close` 时反注册——`UnixRegistry::unregister(path_)` 需要当初 bind 的那个名字。`kUnixPathMax = 108` 字节的定长数组,freestanding 内核不动态分配。

## listen:就翻一个 bool

`listen`(`unix_socket.cpp:128-138`)短得几乎不像个函数:

```cpp
cinux::lib::ErrorOr<void> UnixSocket::listen(int /*backlog*/) {
    auto g = lock_.irq_guard();
    if (!bound_ || listening_ || connected_) {
        return cinux::lib::Error::InvalidArgument;
    }
    listening_ = true;
    return {};
}
```

注释点明跟 TCP 的差别(`unix_socket.cpp:133-135`):TCP 的 `listen` 要去协议模块登记自己(让收到 SYN 时能找到这个 listener);`AF_UNIX` 不用——名字已经在 `bind_path` 时进了 `UnixRegistry`,`connect_path` 通过 registry 就能找到 server,`listen` 只是把「愿意 accept」这个意图标出来。`backlog` 参数收下但忽略(队列深度是编译期常量 `kAcceptMax = 4`)。
