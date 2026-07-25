---
title: Lab 005 · AF_UNIX loopback echo:把 send/accept/recv 串成 round-trip
---

# Lab 005 · AF_UNIX loopback echo:把 send/accept/recv 串成 round-trip

> 005 章把 `AF_UNIX` 的源码过了一遍:`bind_path`/`connect_path` 加法虚函数、`UnixSocket` 两角色一类、`UnixRegistry` 内存命名空间、立即建连无握手。这个 lab 不发答案,只给路径——咱们按「读 → 改 → 跑」三步,亲手让 loopback echo round-trip 在 Book 工作树上端到端跑通,顺手验证几条 005 章声明的边界(send-before-accept、close 撤名、负路径错误码)。所有断言挂在 `kernel/test/test_socket.cpp` 现有测试上,不需要新写测试文件。

## 你要确认的事

开始之前,先在 Book 工作树(`/home/charliechen/Cinux`)上核这几样源码都在、行号对得上 005 章:

1. `kernel/net/unix_socket.{cpp,hpp}` 存在,`UnixSocket` 类声明在 `unix_socket.hpp:109-182`。
2. `kernel/net/unix_registry.cpp` 存在(注意:**没有** `unix_registry.hpp`,`UnixRegistry` 类声明在 `unix_socket.hpp:72-99`)。
3. `kernel/syscall/sys_socket.cpp` 存在(路径是 `syscall/`,**不是** `sys/`),`sys_socket` 的 `AF_UNIX` 分支在 `:223-226`。
4. `kernel/test/test_socket.cpp` 里 `RUN_TEST(test_socket::test_unix_socket_loopback_echo)` 注册在 `:539`。

```bash
# 在 Book 工作树根目录跑
ls kernel/net/unix_socket.cpp kernel/net/unix_socket.hpp kernel/net/unix_registry.cpp
ls kernel/syscall/sys_socket.cpp
grep -n "test_unix_socket_loopback_echo" kernel/test/test_socket.cpp
```

如果上面四样有对不上的(比如 `unix_registry.hpp` 不存在是正常的、`sys_socket.cpp` 不在 `sys/` 下),先回头核路径——005 章的链接全是 `kernel/syscall/sys_socket.cpp`,别被旧 dev note 误导。

## 第一步:读懂 loopback echo 的时序

`test_unix_socket_loopback_echo`(`test_socket.cpp:294-344`)是 005 章的 punchline。先读一遍,搞清四方向的时序:

```bash
sed -n '294,344p' kernel/test/test_socket.cpp
```

重点看三处时序:

- `client.connect_path("/unix_echo")` 在 `:302`,`accept` 在 `:313` —— connect 先、accept 后。
- `client.send(msg, ...)` 在 `:308`,**在 accept 之前**。这是 005 章声明的「立即建连,send 可先于 accept」卖点。字节缓冲在 child 的 4 KB RX 环里,等 accept 把 child 取走。
- echo 回程 `child->send(rbuf, ...)` 在 `:330`,`client.recv(cbuf, ...)` 在 `:334` —— 反方向同一条 send→recv 链路。

**思考题**(自己答,别往下翻答案):如果 `connect_path` 不是立即建连、而是像 TCP 那样阻塞到 accept,这个 send-before-accept 还能成立吗?为什么?(提示:child 此刻还没被 wire 出来。)

## 第二步:把 send-before-accept 这条边界亲手验证一次

005 章声明「立即建连,send 可先于 accept」——别光信书,自己改一次测试看会不会破。**临时**改 `test_unix_socket_loopback_echo`,把 send 挪到 accept **之后**:

```bash
# 临时改动:把 :308 的 send 挪到 :313 的 accept 之后
# (改完跑测试,验完务必 git checkout 还原)
```

具体改法:把 `:308-310` 的 `client.send(msg, sizeof(msg))` 三行剪下来,贴到 `:313` 的 `server.accept(...)` 之后、`:318` 的 `child->recv(...)` 之前。改完跑 socket 测试:

```bash
# 跑 socket 测试的具体命令取决于 Cinux 的测试入口
# 一般是 make test_socket 或进入 kernel/test 目录跑
# 看一眼 kernel/test/Makefile 或 README 找入口
```

预期:测试**仍然过**——send-before-accept 只是 005 章强调的卖点(立即建连让 send 能先发),不是 round-trip 的必要条件。把 send 挪到 accept 之后,child 已经被取出来了,send 拷进 child 的环、recv 立刻取走,round-trip 一样通。这反过来证明 `AF_UNIX` 的连接模型比 TCP 宽松——TCP 得先 accept 才能 send,`AF_UNIX` 两个顺序都行。

验完**务必还原**:

```bash
git checkout kernel/test/test_socket.cpp
```

如果你忘了还原,后面跑全量测试会发现这个文件脏——005 章的源码真值是 send-before-accept,别把临时的验证改动留下来。

## 第三步:验证 close 撤名这条链路

005 章有一节专门澄清「早期 dev note 说 close 无 release 钩子 → 不撤名,这条对当前 Book 源码已过期」。release 钩子补上了(`socket.cpp:148-156`)→ `UnixSocket::close()`(`unix_socket.cpp:295-328`)真做 `UnixRegistry::unregister(path_)`(`:325`)。咱们验证一次「close 后重 bind 同名能成功」。

但 `test_socket.cpp` 现有的测试没直接覆盖这条链路(`test_unix_socket_bind_duplicate` 只测同名 bind 失败,没测 close 后重 bind)。所以这个验证靠**读源码 + 推理**,不写新测试:

1. 读 `UnixSocket::close()`(`unix_socket.cpp:295-328`),确认 `:324-326` 的 `if (was_listening && bound_)` 分支调 `UnixRegistry::instance().unregister(path_)` + `bound_ = false`。
2. 读 `UnixRegistry::unregister`(`unix_registry.cpp:80-89`),确认它把 `entries_[i].used = false`、`path[0] = '\0'`、`sock = nullptr`。
3. 读 `UnixRegistry::register_listener`(`unix_registry.cpp:44-66`),确认它 `find_locked(path) >= 0` 才返 `AlreadyExists`——也就是说 `used = false` 的槽位 `find_locked` 跳过(`:26-28` 的 `if (!entries_[i].used) continue;`),后续 register 同名能进空槽。

推一遍:server bind `/un_x` → close → `unregister` 把槽位 `used = false` → 再 bind `/un_x` → `find_locked` 跳过那个 `used = false` 的槽、找不到 → register 进新槽(或同一个槽),成功。链路通。

**思考题**:为什么 `close` 里只在 `was_listening && bound_` 时才 `unregister`?已连接 socket(client 端、accepted child)的 close 为什么不需要撤名?(提示:它们从来没进过 registry,只有 listener bind 时登记过。)

## 第四步:跑负路径测试,核对错误码

`test_unix_socket_connect_unbound`(`test_socket.cpp:346-351`)和 `test_unix_socket_bind_duplicate`(`test_socket.cpp:353-360`)是两个负路径测试。读一遍:

```bash
sed -n '346,360p' kernel/test/test_socket.cpp
```

核两件事:

- connect 一个没 bind 的名字(`/unix_nobody`)→ 期望 `cinux::lib::Error::NotFound`。这映射到 `sys_connect` 的 `-cinux::to_errno(NotFound)`,在 Linux 用户态是 `-ENOENT`。理由:`connect_path`(`unix_socket.cpp:82-87`)在 `UnixRegistry::lookup` miss 时拿到 `NotFound`。
- bind 同名两次 → 第二次期望 `AlreadyExists`。映射到 `-EEXIST`。理由:`register_listener`(`unix_registry.cpp:49-50`)在 `find_locked(path) >= 0` 时返 `AlreadyExists`。

跑一遍这两个测试,确认都过。如果 connect_unbound 返的不是 `NotFound`(比如返 `InvalidArgument`),那说明 `connect_path` 的错误路径走错了——回头读 `unix_socket.cpp:75-90` 看 lookup miss 是不是正确传播。

## 第五步:核对 copy_from_user 的 陷阱

005 章有一节讲 ring0 测试内核喂不了用户地址给 `sys_bind`,所以 echo 测试走 `UnixSocket` 直接方法。亲手核一遍这个 陷阱 的根源:

```bash
sed -n '60,62p' kernel/arch/x86_64/paging_config.hpp
sed -n '66,79p' kernel/arch/x86_64/user_access.hpp
```

确认:

- `is_user_vaddr`(`paging_config.hpp:60-62`)的判据是 `!(virt & (1ULL << 47))`——bit 47 = 0 才是用户地址。
- `access_ok`(`user_access.hpp:66-79`)在 `a == 0` 或两端点任一不是用户 vaddr 时返 false。
- ring0 测试内核跑在内核高半区(bit 47 = 1),栈指针过不了 `access_ok` → `copy_from_user` 返 false → `parse_sockaddr_un` 返 false → `sys_bind` 返 `-kEfault`(`sys_socket.cpp:244-246`)。

**思考题**:为什么 `test_unix_socket_returns_fd`(`test_socket.cpp:276-292`)能走 `sys_socket`,而 echo 测试不能走 `sys_bind`?(提示:`sys_socket` 的参数是三个整数,不触发 `copy_from_user`;`sys_bind` 的 sockaddr 要从用户态拷。)

## 收尾:把 005 章的声明逐条对上

跑完上面五步,回头逐条核 005 章的「咱们要点亮什么」七条,每条都能在源码或测试里找到证据:

| 005 章声明 | 证据位置 |
|---|---|
| 加法非破坏:`bind_path`/`connect_path` 默认 NotImplemented | `socket.hpp:118-119` 声明、`socket.cpp:33-38` 默认实现 |
| 两角色一类:listening/connected bool 切换 | `unix_socket.hpp:159-163` 状态字段 |
| 内存命名空间 UnixRegistry | `unix_socket.hpp:72-99` 声明、`unix_registry.cpp:16-89` 实现 |
| 立即建连,send 可先于 accept | `unix_socket.cpp:75-126` connect_path + `test_socket.cpp:308`(send 在 accept 前) |
| copy_from_user 的 陷阱 | `paging_config.hpp:60-62` + `user_access.hpp:66-79` + `test_socket.cpp:271-275` 注释 |
| 阻塞抄 prepare_to_wait | `unix_socket.cpp:268-280`(recv 段)、`:174-187`(accept 段) |
| 加锁顺序无 AB-BA | `unix_socket.cpp:57-58`(bind 不嵌套)、`:79-87`(connect 先查 registry 放掉) |

七条全对上,005 章的「教程即验证」才算闭环。如果某条对不上(比如 grep 不到 `prepare_to_wait`、或者 `bind_path` 的行号偏了),回头读 005 章对应小节,看是源码演进了还是章节写错了——教程是 tag-bound 的,以当前工作树的源码真值为准。

### 进一步的折腾(可选)

- 把 `kRxSize`(`unix_socket.hpp:155`)从 4096 改成 8,跑 echo 测试——4 字节消息还是能过,但 echo 回程 + 任何稍大的 payload 就会撞环满的 `WouldBlock` 路径(`unix_socket.cpp:227-233`)。这能让你亲手触发 send 侧流控的 follow-up 边界。
- 读 `test_socketpair_roundtrip`(`test_socket.cpp:500-524`)和 `pair_with`(`unix_socket.cpp:370-385`),对比 `pair_with` 和 `connect_path` 的差别——`pair_with` 是 connect_path 的 peer wiring 减掉 registry/accept 队列那两步,因为 socketpair 两端是同时造的、没有 server/listener。
- 读 `poll_events`(`unix_socket.cpp:391-429`),对比 listening 和 connected 两种角色报的就绪掩码差别(listening 看 accept 队列、connected 看 RX 环 + `POLLHUP`)。这跟 004 `TcpSocket` 的 poll 是镜像。

这些折腾不要求做完,挑一个顺眼的深挖。005 章主线是 bind/connect/echo round-trip,这几个延伸是「顺手吃下的扩展 ABI」的入口。