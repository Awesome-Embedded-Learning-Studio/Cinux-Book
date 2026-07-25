---
title: 07 · 验证、没做的与小结
---

# 验证、没做的与小结

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
