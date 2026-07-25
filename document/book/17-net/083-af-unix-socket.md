---
title: 083 · AF_UNIX:给本地 IPC 走一条不走网卡的近道
---

# 083 · AF_UNIX:给本地 IPC 走一条不走网卡的近道

> 到 069 章为止,`AF_INET` 这条腿已经站得很稳——UDP(063)、TCP(069)、协议栈、e1000 驱动全跑通了。可 Linux 的 socket 抽象里还有**另一条腿**:`AF_UNIX`,本地命名空间 socket。它服务的场景跟 `AF_INET` 完全不同——两个任务在同一台机器上要传字节流,走 TCP 得分配端口、填 IPv4 地址、过协议栈、包再到 loopback 收一圈,纯是杀鸡用牛刀。`AF_UNIX` 给的是一条**不走网卡的近道**:socket 还是那个 socket(同样的 `socket()`/`bind()`/`listen()`/`accept()`/`send()`/`recv()`),只是地址从 `(IP, port)` 换成了一个**文件系统风格的名字**,字节不经网卡、不经 L4 协议栈,直接从一个 socket 的 `send()` 拷到对端 socket 的 RX 环里。
>
> 这一章的真主题不是「AF_UNIX 字段怎么填」——那部分比 TCP 还简单,因为根本没有协议字段要填。真正要讲清的是三件**加法而非破坏**的工程决定:其一,`AF_UNIX` 的路径名字塞不进 `AF_INET` 那对 `bind(uint16_t)`/`connect(Ipv4Addr, port)` 的既有签名,于是给 Socket base **加一对 `bind_path`/`connect_path` 虚函数**,默认 `NotImplemented`,既有 Udp/TcpSocket 一行不改;其二,一个 `UnixSocket` 类同时扮演**监听端和已连接端两个角色**(跟 069 的 `TcpSocket` 同一套镜像思路),靠 `listening_`/`connected_` 几个 bool 切换;其三,`AF_UNIX` 的名字住在一个**内存命名空间**(`UnixRegistry`,一张定长表),**不依赖任何真文件系统**——bind 不是去 tmpfs/ext2 上建 socket inode,是往内存表里塞一条记录。一条诚实的边界先说在前头:这是 loopback 友好的最小可用 `AF_UNIX`,真 Linux 的 fs-backed bind、abstract socket(`path` 首字节 `\0`)、DGRAM `sendto(path)` 都还没做;`getsockname`/`getpeername`/`socketpair`/`shutdown`/`poll` 这几样已经顺手吃下了,正文点到、不喧宾夺主。

## 这章咱们要点亮什么

1. **加法非破坏**:`bind_path`/`connect_path` 是 Socket base 上新加的虚函数,默认 `NotImplemented`。不是去改 `AF_INET` 那对 `bind(uint16_t)`/`connect(Ipv4Addr, port)` 的签名(那会撞已经合进 main 的 UdpSocket/TcpSocket),而是给 `AF_UNIX` 单开一对路径形状的入口——`sys_bind`/`sys_connect` 按 `domain()` 派发。
2. **两角色一类**:`UnixSocket` 既是监听 socket(server:bind + listen + accept),又是已连接 socket(client connect 之后、或 accept 出来的 child)。`listening_`/`connected_` 两个 bool 区分角色,跟 069 的 `TcpSocket` 镜像同款。
3. **内存命名空间 `UnixRegistry`**:`AF_UNIX` 的名字住在一张定长表里(`kUnixRegistryMax = 16`),bind 就是塞记录、connect 就是查记录、close 就是删记录。**不碰真 fs**。镜像 071 那个 `FifoRegistry`。
4. **立即建连,无内核握手**:`connect_path` 直接 new 一个 child、把 client 和 child 互指为 peer、把 child 塞进 server 的 accept 队列。没有 SYN/SYN-ACK 那一套(那是 069 TCP 才需要的协议握手)。所以确定性单线程测试里,**`send` 可以先于 `accept`**——字节先缓冲在 child 的 RX 环里,等 `accept` 把 child 取走。
5. **`copy_from_user` 的陷阱**:ring0 测试内核没法给 `sys_bind` 喂一个「用户地址」——`copy_from_user` 的 `is_user_vaddr` 范围检查拒内核栈指针,`sys_bind` 会 `-EFAULT`。所以 echo 测试走 `UnixSocket` 的**直接方法**(`bind_path`/`send`/`recv`),不走 `sys_bind`。这不是 `UnixSocket` 的 bug,是测试侧的局限,生产路径留 musl 覆盖。
6. **阻塞抄真调度,不 sti/hlt**:recv 空环、accept 队列空,都 `prepare_to_wait` + `schedule_blocked` 真睡,`send`/`enqueue_accept`/`close` 用 `wake_one`/`wake_all` 叫醒。跟 071 修 pipe 阻塞、059 首 discovered 的那族 `sti`-in-syscall `#DF` 同根,这里直接复用 `prepare_to_wait` 模板,不碰 `sti`/`hlt`。
7. **加锁顺序无 AB-BA**:`UnixRegistry` 的锁**永远不嵌套**在 socket 锁里。`bind_path`/`connect_path` 先拿 registry 锁查/登记、**放掉**,再单独拿 socket 锁。peer 指针是 write-once(connect/accept 时设一次之后不动),`send` 拷进对端 RX 环时只拿**对端那一把锁**,从不同时拿两把 socket 锁。

## 为什么 AF_UNIX 是 socket family 的第三个成员

073 立起来那个 socket 适配器模子——`SocketOps` 这个 `InodeOps` 子类 + 一个共享单例 `socket_ops()`——已经把「socket fd 就是个 pipe fd」这件事落定了:`File -> Inode -> SocketOps`,`sys_read`/`sys_write`/`sys_close`/`sys_ioctl` 全走 `InodeOps` 派发,**fd 层零改**。`AF_INET` 的 UDP/TCP 挂在这个模子上;`AF_UNIX` 是挂同一个模子上的**第三个 socket family**,差别只在 `Socket` 子类是谁、`bind`/`connect` 走哪条虚函数路径。

源码侧,`sys_socket` 的派发就这么直接(`sys_socket.cpp:212-235`):

```cpp
if (domain == static_cast<uint64_t>(kAfUnix)) {
    Socket* s = new UnixSocket(static_cast<int>(type));
    return install_socket_fd(s);
}
if (domain != static_cast<uint64_t>(kAfInet)) {
    return -cinux::kEafnosupport;
}
Socket* s = create_socket(static_cast<int>(domain), static_cast<int>(type));
```

注意这个分支顺序的小心思:`AF_UNIX` 在 `create_socket()` **之前**就被截走直接 `new UnixSocket` 了。原因写在 `sys_socket.cpp:218-222` 的注释里——`create_socket()` 是通往生产 L4 协议栈(`AF_INET`)的**唯一**桥梁,协议栈没起来(NIC 没初始化)时它返 `nullptr`;而 `AF_UNIX` 自给自足,不依赖 NIC、不依赖 L4 模块、不需要 `net_init`。把 `AF_UNIX` 截在 `create_socket` 之前,就让它在**测试内核**(没有 `net_init`)里也能用,而且不用为了加 `AF_UNIX` 去动 `drivers/net` 和 `net_stub` 两个地方——典型的「加法非破坏」:`AF_INET` 那条路径一行没改。

`kAfUnix = 1` 这个常量在 `socket.hpp:38` 跟 `kAfInet = 2` 并排:

```cpp
constexpr int kAfUnix     = 1;  ///< AF_UNIX (local IPC) -- F8-M3
constexpr int kAfInet     = 2;  ///< AF_INET (IPv4)
```

值跟 Linux 用户态对齐(用户态 musl 编出来的 `AF_UNIX` 也是 1),所以同一份 `socket(AF_UNIX, SOCK_STREAM, 0)` 调用,在 Cinux 和在 Linux 上语义一致。

## 加法非破坏:bind_path / connect_path 这对虚函数

`AF_UNIX` 的地址是一个**路径字符串**(比如 `/unix_echo`),跟 `AF_INET` 的 `(Ipv4Addr, port)` 形状完全对不上。要把它塞进既有签名,有两条路:

- 改 `bind`/`connect` 的签名,让它们既吃 `(uint16_t)` 又吃 `(const char*)`——blast radius 巨大,会动到已经合进 main 的 `UdpSocket`/`TcpSocket`,而且把一个 IP 协议形状的接口硬扭成泛用,接口味道很差。
- **加一对新虚函数** `bind_path(const char*)`/`connect_path(const char*)`,默认 `NotImplemented`,只让 `UnixSocket` override。

源码选了第二条(`socket.hpp:117-120`):

```cpp
/// @name AF_UNIX (path-based) API -- overridden by UnixSocket (F8-M3).
virtual cinux::lib::ErrorOr<void>    bind_path(const char* path);
virtual cinux::ErrorOr<void>    connect_path(const char* path);
```

默认实现就是返 `NotImplemented`(`socket.cpp:33-38`):

```cpp
cinux::lib::ErrorOr<void> Socket::bind_path(const char* /*path*/) {
    return cinux::lib::Error::NotImplemented;
}
cinux::lib::ErrorOr<void> Socket::connect_path(const char* /*path*/) {
    return cinux::lib::Error::NotImplemented;
}
```

这个范本 `socket.hpp:107-120` 整段都在用——`bind`/`connect`/`listen`/`accept`/`send`/`recv` 全是 default-stub 虚函数,子类按需 override,最早这范本就是为了让 `socket()` + `close()` 先跑起来,协议 op 后面慢慢补。`ioctl`/`stat`/`open` 也是同一个 default-virtual 套路(继承 `InodeOps` 默认返 `NotImplemented`/`-1`/`false`)。所以 `bind_path`/`connect_path` 不是异类,是这套范本的自然延伸。

派发在 `sys_bind`/`sys_connect` 里(`sys_socket.cpp:242-249` / `264-271`):

```cpp
// sys_bind
if (s->domain() == kAfUnix) {
    char path[cinux::net::kUnixPathMax];
    if (!parse_sockaddr_un(addr, addrlen, path)) {
        return -cinux::kEfault;
    }
    auto r = s->bind_path(path);
    return r.ok() ? 0 : -cinux::to_errno(r.error());
}
```

`sys_connect` 长得几乎一样,只是调 `connect_path`。注意 `kAfUnix` 这个分支**只调 path 那对虚函数**,`AF_INET` 走下面 `parse_sockaddr_in` + `s->bind(port)` 的老路——两条腿完全分开,谁也不挡谁。

`listen`/`accept`/`send`/`recv` 不分 family,直接调 Socket base 上的 family-agnostic 虚函数(`sys_socket.cpp:281-288` 的 `sys_listen`、`sys_socket.cpp:174-210` 的 `do_accept`)。唯一一处 family 特判在 `do_accept` 末尾(`sys_socket.cpp:205-208`):

```cpp
// AF_UNIX peers carry no address/port -- leave the caller's sockaddr untouched.
if (s->domain() != kAfUnix) {
    fill_sockaddr_in(addr, addrlen_ptr, remote, rport);
}
```

`AF_UNIX` 的 peer 没有地址/端口可言(就是个匿名 socket),所以 `accept` 回填 sockaddr 这步直接跳过——又一处「family 差异在最小处特判」的体现。

### sockaddr_un 怎么解析

`parse_sockaddr_un`(`sys_socket.cpp:116-135`)把用户态的 `sockaddr_un` 拷进内核、校验、NUL-终止:

```cpp
bool parse_sockaddr_un(uint64_t addr_virt, uint64_t addrlen, char* out_path) {
    if (addr_virt == 0 || addrlen < 2) {  // need at least the family field
        return false;
    }
    SockAddrUn sa;
    if (!copy_from_user(&sa, reinterpret_cast<void*>(addr_virt), sizeof(sa))) {
        return false;
    }
    if (sa.family != kAfUnix) {
        return false;
    }
    // Force NUL-termination regardless of what userspace wrote.
    uint32_t i = 0;
    while (i + 1 < cinux::net::kUnixPathMax && sa.path[i] != '\0') {
        out_path[i] = sa.path[i];
        ++i;
    }
    out_path[i] = '\0';
    return out_path[0] != '\0';  // Linux allows empty (abstract) paths; we do not yet
}
```

三步:**长度校验**(至少要能装下 2 字节的 `family`)→ `copy_from_user` 拷 `SockAddrUn` 进来(110 字节,`family` + `path[108]`)→ 强制 NUL-终止。最后一行 `return out_path[0] != '\0'` 是边界声明:Linux 允许 path 首字节为 `\0` 的「abstract socket」(名字不走文件系统,住在内核的一个特殊表里),Cinux 这会儿**不做**——空 path 直接当非法拒掉。这条留到「这章没做的」里。

`SockAddrUn` 结构跟常量在 `socket.hpp:62-71`:

```cpp
struct SockAddrUn {
    uint16_t family;     ///< AF_UNIX (host order)
    char     path[108];  ///< NUL-terminated leaf name
};
static_assert(sizeof(SockAddrUn) == 110, "sockaddr_un is family(2) + path[108]");

static constexpr uint32_t kUnixPathMax = 108;
```

`kUnixPathMax = 108` 这个边界常量**住在 `socket.hpp`,不 in `unix_socket.hpp`**(`unix_socket.hpp:57-58` 的注释特意点明这一点)——理由是 `sys_socket.cpp`(派发 + `parse_sockaddr_un`)和测试都要用同一个值,放 `SockAddrUn` 旁边能让每个看见这个结构体的翻译单元都顺手看见它的边界,不会出现「结构体在 A 头、长度在 B 头、两边各猜一个数」的失同步。

## 内存命名空间:UnixRegistry

`AF_UNIX` 的名字不住在任何真文件系统上——不是去 tmpfs 建 socket inode,也不是去 ext2 建特殊文件,而是住在一张**进程级的内存表**里。这就是 `UnixRegistry`(`unix_socket.hpp:72-99`):

```cpp
class UnixRegistry {
public:
    static UnixRegistry& instance();
    cinux::lib::ErrorOr<void>        register_listener(const char* path, UnixSocket* sock);
    cinux::lib::ErrorOr<UnixSocket*> lookup(const char* path);
    void                             unregister(const char* path);
private:
    struct Entry {
        char        path[kUnixPathMax];
        bool        used{false};
        UnixSocket* sock{nullptr};
    };
    Entry                 entries_[kUnixRegistryMax];
    cinux::proc::Spinlock lock_;
    int find_locked(const char* path) const;
};
```

定长表(16 条)、`Spinlock`、按 leaf-name 比较——这套范本 071 立过(`FifoRegistry`),`AF_UNIX` 这里照搬。`register_listener` 塞记录、`lookup` 查记录、`unregister` 删记录(`unix_registry.cpp:44-89`):

```cpp
// register_listener: 已存在 -> AlreadyExists;表满 -> OutOfMemory
auto g = lock_.guard();
if (find_locked(path) >= 0) {
    return cinux::lib::Error::AlreadyExists;
}
for (uint32_t i = 0; i < kUnixRegistryMax; ++i) {
    if (!entries_[i].used) {
        // ... 复制 path、标 used、挂 sock ...
        return {};
    }
}
return cinux::lib::Error::OutOfMemory;  // table full
```

`find_locked` 是 caller 持锁调的字节比较(`unix_registry.cpp:21-42`),就是双指针扫到双方都 `\0` 为止的朴素 strcmp。没有 `<map>`、没有 `<string>`——freestanding 内核用不上 STL 容器,定长表 + 自写比较是 Cinux 一贯的选择(跟 `FifoRegistry`、TTY 表、PTY 表全一个味道)。

### 为什么不走真 fs

`unix_socket.hpp:5-9` 文件头注释把这条边界讲得很直白:

> `AF_UNIX` gives two tasks a byte-stream IPC channel addressed by a filesystem-style path -- but the path lives in an IN-MEMORY namespace, not on tmpfs/ext2. A real filesystem-backed `bind()` (create/unlink a socket inode on a mounted fs) is a documented follow-up; this milestone keeps the kernel off a filesystem dependency (hobby-OS simplification, mirroring `ipc::FifoRegistry`).

真 Linux 的 `AF_UNIX bind("/tmp/foo.sock")` 会在 `/tmp` 上建一个 socket inode,`connect` 走 vfs 路径解析找到它,`unlink` 删掉——整套跟文件系统耦合。Cinux 这会儿不背这个依赖:`bind` 是往内存表塞记录,`connect` 是查内存表,跟 vfs 完全脱钩。代价是 socket 名字**重启即丢**(进程级单例,不持久化),好处是 `AF_UNIX` 不依赖任何已挂载的 fs,在内核启动早期、根 fs 还没挂的时候也能用。这是「loopback 友好的最小可用」的诚实取舍。

### 单例只此一个

`UnixRegistry::instance()`(`unix_registry.cpp:16-19`)是经典的 Meyers singleton:

```cpp
UnixRegistry& UnixRegistry::instance() {
    static UnixRegistry reg;
    return reg;
}
```

一个进程只有一个 `AF_UNIX` 命名空间——跟 Linux 默认一致(Linux 的 mount namespace 才让不同进程看到不同 fs,`AF_UNIX` 名字空间的隔离是后面的事)。`bind_path` 登记进这个单例,`connect_path` 查这个单例,`close` 从这个单例删——三处都通过 `instance()` 拿到同一张表。

## 两角色一类:UnixSocket 的 listening 与 connected

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

### bind_path:登记 + 留名

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

### listen:就翻一个 bool

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

## connect_path:立即建连,无内核握手

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

### 立即建连意味着 send 可以先于 accept

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

### enqueue_accept:塞队列 + 叫醒 accept'er

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

## send 与 recv:拷进对端环,排空自己环

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

### send 侧流控是 follow-up

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

## 验证:loopback echo round-trip

「教程即验证」——这一章写的过程就是把 `AF_UNIX` 真在 Book 工作树上端到端跑通。验证分四层:

**第一层:socket() 真能造出 SocketOps fd。** `test_unix_socket_returns_fd`(`test_socket.cpp:276-292`)走 `sys_socket(AF_UNIX, SOCK_STREAM, 0)`,断言返回的 fd 是 `SocketOps` inode、`domain()` 是 `kAfUnix`、`type()` 是 `kSockStream`,再 `sys_close` 干净关闭:

```cpp
int64_t fd = sys_socket(kAfUnix, kSockStream, 0, kFiller, kFiller, kFiller);
TEST_ASSERT_GE(fd, 0);
cinux::fs::File* f = current_fd_table().get(static_cast<int>(fd));
TEST_ASSERT_EQ(f->inode->ops, &socket_ops());  // 是 socket fd
auto* s = static_cast<Socket*>(f->inode->fs_private);
TEST_ASSERT_EQ(s->domain(), kAfUnix);
TEST_ASSERT_EQ(s->type(), kSockStream);
TEST_ASSERT_EQ(sys_close(...), 0);
```

**第二层:loopback echo round-trip。** `test_unix_socket_loopback_echo`(`test_socket.cpp:294-344`)是这一章的 punchline。server bind + listen,client connect,**先 send 后 accept**(立即建连的卖点),server accept 出 child、recv 出字节、echo 回去,client recv 回程——四个方向全断言:

```cpp
static UnixSocket server(kSockStream);
static UnixSocket client(kSockStream);
TEST_ASSERT_TRUE(server.bind_path("/unix_echo").ok());
TEST_ASSERT_TRUE(server.listen(1).ok());
TEST_ASSERT_TRUE(client.connect_path("/unix_echo").ok());

static const uint8_t msg[] = {'u', 'n', 'i', 'x'};
auto sr = client.send(msg, sizeof(msg));          // send BEFORE accept
TEST_ASSERT_TRUE(sr.ok());

auto acc = server.accept(nullptr, nullptr);        // accept 取 child
Socket* child = *acc;
uint8_t rbuf[8] = {};
auto rr = child->recv(rbuf, sizeof(rbuf), nullptr, nullptr);
TEST_ASSERT_EQ(*rr, static_cast<int64_t>(sizeof(msg)));
// payload 逐字节比对
// echo back: child -> client
auto er = child->send(rbuf, static_cast<uint32_t>(sizeof(msg)));
auto cr = client.recv(cbuf, sizeof(cbuf), nullptr, nullptr);
TEST_ASSERT_EQ(*cr, static_cast<int64_t>(sizeof(msg)));
```

注意 `server`/`client` 是 `static`——块注释 `test_socket.cpp:295-296` 说明:4 KB RX 环让每个 `UnixSocket` 占 4 KB+,两个放栈上会爆 16 KB 内核栈,所以用 `static`(跟 069 TcpSocket echo 测试同款妥协)。echo round-trip 走通,证明 `bind_path` → `listen` → `connect_path`(立即建连 + wire peer + enqueue child)→ `send`(拷进对端环)→ `accept`(取 child)→ `recv`(排空环)→ 回程 `send`/`recv` 整条链路端到端对。

**第三层:负路径。** 两个负向测试守住错误语义:

- `test_unix_socket_connect_unbound`(`test_socket.cpp:346-351`):connect 一个没 bind 的名字 → `NotFound`(映射 `ENOENT`)。验证 registry miss 的错误路径。
- `test_unix_socket_bind_duplicate`(`test_socket.cpp:353-360`):bind 同名两次 → 第二次 `AlreadyExists`(映射 `EEXIST`)。验证 registry 的去重。

**第四层:扩展 ABI 已落地。** 这一章主要讲 STREAM 的 bind/connect/listen/accept/send/recv 六个原语,但 `AF_UNIX` 顺手吃下了几样扩展 ABI(后续补的,源码已带入 Book):

- `getsockname`/`getpeername`:`get_local_addr`/`get_peer_addr`(`unix_socket.cpp:334-368`)。`test_unix_getsockname_local_addr`(`test_socket.cpp:451-467`)断言 bound path 回填正确;`test_unix_getpeer_addr_connected`(`test_socket.cpp:469-480`)断言 connected 后 peer 名字存在(匿名,family 对、path 空)。
- `socketpair(2)`:`pair_with`(`unix_socket.cpp:370-385`)wire 两个未连接 socket 互为 peer,绕开 registry/accept 队列——这是 `connect_path` 的 peer wiring 减掉登记那步。`test_socketpair_roundtrip`(`test_socket.cpp:500-524`)走 `do_socketpair_kernel` 造一对 fd,经 `InodeOps::write`/`read` 端到端验字节往返。
- `shutdown`:`do_shutdown`(`socket.hpp:145-147`)记录方向 bit,`send`/`recv` 入口查 `shut_write()`/`shut_read()`(`unix_socket.cpp:206`/`:248`)。`test_shutdown_directions`(`test_socket.cpp:482-498`)断言 `SHUT_WR` 后 send 返 `BrokenPipe`、`SHUT_RD` 后 recv 返 0(EOF)。
- `poll(2)`/`select(2)`:`poll_events`/`poll_detach_waiter`(`unix_socket.cpp:391-429`)。listening 报 accept 队列就绪、connected 报 RX 环就绪 + `POLLHUP`(对端 EOF),镜像 `TcpSocket`。

这四样不展开讲——它们挂在 073 立的 socket 适配器模子上,跟 `AF_INET` 的 Udp/Tcp 共用同一套 `Socket` base 接口。本章主线是 bind/connect/echo round-trip,这几样点到「已落地 + 有测试」即可,不喧宾夺主。RUN_TEST 注册在 `test_socket.cpp:538-541`(AF_UNIX 核心 4 例)+ `:548-551`(getsockname 等 4 例),加上 setsockopt/getsockopt/accept4 又 4 例,`AF_UNIX` 总共撑起了 socket syscall 对齐的一大片测试。

## 这章没做的

诚实的边界清单:

1. **fs-backed bind**:`AF_UNIX` 名字只住内存表,不在 tmpfs/ext2 上建 socket inode。真 Linux 的 `bind("/tmp/foo.sock")` 会在 fs 上造一个 `S_IFSOCK` 节点,`connect` 走 vfs 路径解析——Cinux 这会儿不碰 fs,bind 是往 `UnixRegistry` 塞记录。
2. **abstract socket**:`path` 首字节 `\0` 的 Linux abstract namespace(名字不走 fs、住内核特殊表),`parse_sockaddr_un`(`sys_socket.cpp:134`)显式拒——`return out_path[0] != '\0'`。
3. **DGRAM `sendto(path)`**:`socket(AF_UNIX, SOCK_DGRAM, ...)` 收下(type 校验过),但 `sendto` 带显式 path 的 DGRAM 语义没做(`sendto` 的 `AF_UNIX` 分支没写)。STREAM 够用,DGRAM 留后续。
4. **send 侧阻塞流控**:环满时 `send` 返 `WouldBlock`,不睡 write-waiters(没有这个队列)。真 Linux 阻塞 socket 会睡到对端 drain,Cinux 这儿返 `EAGAIN` 让调用方自己退避。
5. **connect 阻塞语义**:`connect_path` 立即建连返回,不阻塞到 server `accept`(真 Linux STREAM `connect` 是阻塞到 accept 的)。loopback 友好的简化,确定性单线程测试受益。
6. **SMP peer 可见性 happens-before**:peer_ 是 write-once、snapshot-under-lock,单核安全;多核场景下 `peer_->...` 的内存可见性没显式 memory barrier 验证过(Cinux 这会儿单核,问题不显)。
7. **SOCK_NONBLOCK / per-fd 非阻塞 flag**:`accept4` 收 `SOCK_CLOEXEC` 但 `SOCK_NONBLOCK` 只收不接(`sys_socket.cpp:196-198` 注释明说「no per-fd nonblock flag yet」)。

## 小结

`AF_UNIX` 是 Cinux socket family 的第三个成员,挂的方式跟 UDP/TCP 一样轻——`sys_socket` 里一个 `if (domain == kAfUnix)` 分支直接 `new UnixSocket`,`AF_INET` 路径一行没改。三件工程决定撑起这一章:

- **加法非破坏**:`bind_path`/`connect_path` 是 Socket base 上新加的虚函数,默认 `NotImplemented`,既有 Udp/TcpSocket 不动。`AF_UNIX` 的路径形状跟 `AF_INET` 的 `(Ipv4Addr, port)` 形状对不上,加新接口比改既有签名干净——blast radius 为零。
- **两角色一类**:`UnixSocket` 既是监听 socket 又是已连接 socket,`listening_`/`connected_` 几个 bool 切换,跟 069 `TcpSocket` 镜像同款。`accept` 出来的 child 和 client 用同一个类型,`send`/`recv` 一份代码两种角色共用。
- **内存命名空间**:`UnixRegistry` 一张定长表 + Spinlock,bind 塞记录、connect 查记录、close 删记录,不碰真 fs。镜像 071 `FifoRegistry`,进程级单例。

立即建连(无内核握手)是 `AF_UNIX` 跟 TCP 最大的语义差别——`connect_path` 直接 wire 双向 peer + enqueue child,所以 send 可以先于 accept,字节缓冲在 child 的 4 KB RX 环里。阻塞抄 `prepare_to_wait` 真调度模板(071 修对的那套,避开 `sti`/`hlt` 的 `#DF` 坑),加锁顺序无 AB-BA(registry 锁不嵌套在 socket 锁里、peer write-once 让 send 只持一把锁)。`copy_from_user` 的 `is_user_vaddr` 范围检查让 ring0 测试喂不了用户地址,echo 测试走 `UnixSocket` 直接方法——这是测试侧妥协,不是产品缺陷,生产路径留 musl 覆盖。loopback echo round-trip 端到端跑通,加上负路径和扩展 ABI 的测试,`AF_UNIX` 在 Book 真能用。