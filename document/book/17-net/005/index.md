---
title: 005 · AF_UNIX:给本地 IPC 走一条不走网卡的近道
---

# 005 · AF_UNIX:给本地 IPC 走一条不走网卡的近道

> 到 069 章为止,`AF_INET` 这条腿已经站得很稳。这一章给 Linux socket 抽象里的另一条腿 `AF_UNIX`(本地命名空间 socket)铺一条不走网卡的近道——同样的 `socket()`/`bind()`/`listen()`/`accept()`,只是地址从 `(IP, port)` 换成一个文件系统风格的名字,字节不经网卡直接从一个 socket 拷到对端的 RX 环里。

## 本章路线

- [01 · 导引:为什么是第三个成员 + 加法非破坏的 bind_path](01-intro.md)
- [02 · 内存命名空间 UnixRegistry](02-registry.md)
- [03 · 两角色一类:UnixSocket 的 listening 与 connected](03-listen.md)
- [04 · connect_path:立即建连,无内核握手](04-connect.md)
- [05 · send 与 recv:拷进对端环,排空自己环](05-sendrecv.md)
- [06 · close、copy_from_user 陷阱与阻塞模板](06-close-block.md)
- [07 · 验证、没做的与小结](07-wrapup.md)
