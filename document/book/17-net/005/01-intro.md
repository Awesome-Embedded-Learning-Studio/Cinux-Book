---
title: 01 · 导引:为什么是第三个成员 + 加法非破坏的 bind_path
---

# 导引:为什么是第三个成员 + 加法非破坏的 bind_path

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
