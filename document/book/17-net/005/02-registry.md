---
title: 02 · 内存命名空间 UnixRegistry
---

# 内存命名空间 UnixRegistry

## UnixRegistry:一张定长表

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

## 为什么不走真 fs

`unix_socket.hpp:5-9` 文件头注释把这条边界讲得很直白:

> `AF_UNIX` gives two tasks a byte-stream IPC channel addressed by a filesystem-style path -- but the path lives in an IN-MEMORY namespace, not on tmpfs/ext2. A real filesystem-backed `bind()` (create/unlink a socket inode on a mounted fs) is a documented follow-up; this milestone keeps the kernel off a filesystem dependency (hobby-OS simplification, mirroring `ipc::FifoRegistry`).

真 Linux 的 `AF_UNIX bind("/tmp/foo.sock")` 会在 `/tmp` 上建一个 socket inode,`connect` 走 vfs 路径解析找到它,`unlink` 删掉——整套跟文件系统耦合。Cinux 这会儿不背这个依赖:`bind` 是往内存表塞记录,`connect` 是查内存表,跟 vfs 完全脱钩。代价是 socket 名字**重启即丢**(进程级单例,不持久化),好处是 `AF_UNIX` 不依赖任何已挂载的 fs,在内核启动早期、根 fs 还没挂的时候也能用。这是「loopback 友好的最小可用」的诚实取舍。

## 单例只此一个

`UnixRegistry::instance()`(`unix_registry.cpp:16-19`)是经典的 Meyers singleton:

```cpp
UnixRegistry& UnixRegistry::instance() {
    static UnixRegistry reg;
    return reg;
}
```

一个进程只有一个 `AF_UNIX` 命名空间——跟 Linux 默认一致(Linux 的 mount namespace 才让不同进程看到不同 fs,`AF_UNIX` 名字空间的隔离是后面的事)。`bind_path` 登记进这个单例,`connect_path` 查这个单例,`close` 从这个单例删——三处都通过 `instance()` 拿到同一张表。
