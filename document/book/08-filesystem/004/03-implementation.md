---
title: 03 · 实现:sys_open/read/write/getdents、libc、shell
---

# 实现:sys_open/read/write/getdents、libc、shell

### sys_open:路径 → inode → fd

`sys_open` 是整条链路的入口,它的活就是「把用户给的路径,变成一个可用的 fd」,见 [sys_open.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/sys_open.cpp):

```cpp
int64_t sys_open(uint64_t path_virt, uint64_t flags, ...) {
    auto* path = reinterpret_cast<const char*>(path_virt);

    // ① 规范地址检查(防用户传空指针 / 内核地址)
    if (path_virt == 0) return -1;
    uint64_t bit47 = (path_virt >> 47) & 1;
    uint64_t upper = path_virt >> 48;
    if (bit47 == 0 && upper != 0)    return -1;   // 用户态地址,高位必须全 0
    if (bit47 == 1 && upper != 0xFFFF) return -1; // 内核态地址,高位必须全 1
    if (path[0] == '\0') return -1;

    // ② ③ VFS 解析 + 后端查找
    const char* rel_path = nullptr;
    auto* fs = cinux::fs::vfs_resolve(path, &rel_path);
    if (fs == nullptr) return -1;
    auto* inode = fs->lookup(rel_path);
    if (inode == nullptr) return -1;

    // ④ 分配描述符
    int fd = cinux::fs::g_global_fd_table().alloc(inode, /*flags→OpenFlags*/);
    return (fd == cinux::fs::FD_NONE) ? -1 : fd;
}
```

那段规范地址检查值得停一下。x86-64 的虚拟地址必须是「规范形」:bit 47 决定了这是用户态地址(bit47=0,那么 bit 48–63 必须全 0)还是内核态地址(bit47=1,bit 48–63 必须全 1)。用户程序正常传的指针都在用户态低半区(bit47=0、高位全 0)。如果用户恶意(或写错)传了个内核态地址(高位全 1),内核若不检查就 `reinterpret_cast` 去读,等于帮用户读了内核内存——典型的越权。这道检查把「用户态才能传的地址」收在低半区,是系统调用安全的底线。每个收用户指针的调用(open 的 path、read/write/getdents 的 buf)开头都有这道闸。

### sys_read / sys_write:fd → File → InodeOps

`sys_read` 收编进 VFS 后,按 fd 分两条路,见 [sys_read.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/sys_read.cpp):

```cpp
int64_t sys_read(uint64_t fd, uint64_t buf_virt, uint64_t count, ...) {
    /* 规范地址检查 buf_virt ... */

    if (fd == 0) {
        // stdin(fd 0):走老路读 PS/2 键盘——这条不经过 VFS
        // poll 键盘事件,按行返回...
        return read_bytes;
    }

    // fd > 0:VFS 读
    cinux::fs::File* file = cinux::fs::g_global_fd_table().get(static_cast<int>(fd));
    if (file == nullptr || file->inode->ops->read == nullptr) return -1;

    int64_t result = file->inode->ops->read(file->inode, file->offset,
                                            reinterpret_cast<void*>(buf_virt), count);
    if (result > 0) file->offset += static_cast<uint64_t>(result);  // ★ VFS 层推进偏移
    return result;
}
```

为什么 fd 0 要特殊处理?因为 stdin 不是「VFS 里的一个文件」——它没有 inode、不在挂载表里,FDTable 也不给 fd 0 分配 File(003 里 FDTable 从 fd 3 起算,0/1/2 是空的)。stdin 的数据来自键盘,所以 fd 0 的 read 直接走键盘驱动那条老路(014 写的 PS/2 键盘)。而 fd > 0 都是 VFS 里 open 出来的真实文件,走「File → inode 操作」。这两条路并存,是 stdin/stdout 这对「假文件」和真文件的本质差异。

那一行 `file->offset += result` 是 sequential read 能工作的关键。后端的 `read(inode, offset, buf, count)` 是**无状态**的——它只认传进来的 offset,不记得「上次读到哪」。所以「读到哪了」这个状态由 VFS 层的 `File::offset` 持有,每次 read 完由 sys_read 推进。漏了这行,read 会永远从 offset 0 开始,反复返回文件开头那几字节。

`sys_write` 同理(把 fd 1/stdout 这种也收编),`sys_close` 就是 `FDTable.close(fd)`——释放 File、归还 fd 槽位。

### sys_getdents:用 offset 当条目下标

`sys_getdents` 列目录,见 [sys_getdents.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/sys_getdents.cpp):

```cpp
int64_t sys_getdents(uint64_t fd, uint64_t buf_virt, uint64_t count, ...) {
    /* 规范地址检查 buf_virt ... */
    cinux::fs::File* file = cinux::fs::g_global_fd_table().get(static_cast<int>(fd));
    if (file == nullptr || file->inode->ops->readdir == nullptr) return -1;

    auto* name_buf = reinterpret_cast<char*>(buf_virt);
    int64_t result = file->inode->ops->readdir(file->inode, file->offset, name_buf, count);

    if (result == 1) {                 // 读到一个条目
        file->offset++;                // ★ 下标 +1(对目录,offset 就是条目序号)
        uint64_t len = 0;              // 量出名字长度
        while (len < count && name_buf[len] != '\0') ++len;
        return static_cast<int64_t>(len);   // 返回名字长度
    }
    return result;   // 0=目录读完,-1=出错
}
```

这里 `file->offset` 当**目录条目的下标**用(003 的 `ramdisk_readdir` 里,index 0 是 `.`、1 是 `..`、2 起是文件条目)。每次 getdents 读一条、offset++,循环到返回 0 就列完了。返回值是「这次读到的名字长度」(不是字节数、也不是条目数),调用方拿这个长度去切 buf 里的名字。这种「一次一条、靠 offset 推进」的接口,是最朴素的 getdents 形态(POSIX 的 `getdents` 返回的是结构体数组、一次多条,这里简化成一次一条)。

### 用户态 libc 包装

用户态要调这些系统调用,得有地方触发 `syscall` 指令。libc 里给每个调用写了个薄壳,见 [syscall.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/libc/syscall.cpp):

```cpp
static inline int64_t _syscall3(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3) {
    int64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)                              // 返回值在 rax
        : "a"(nr), "D"(a1), "S"(a2), "d"(a3)     // syscall 号在 rax,参数在 rdi/rsi/rdx
        : "rcx", "r11", "memory"
    );
    return ret;
}

int64_t sys_read(int fd, void* buf, size_t count) {
    return _syscall3(static_cast<uint64_t>(SyscallNr::SYS_read),
                     (uint64_t)fd, (uint64_t)buf, (uint64_t)count);
}
```

就是一句内联汇编:把系统调用号放进 `rax`、参数放进 `rdi/rsi/rdx`(x86-64 syscall 调用约定),执行 `syscall` 陷入内核,内核返回后从 `rax` 取结果。`"rcx","r11"` 进 clobber 列表是因为 `syscall` 指令会破坏这两个寄存器(硬件行为,023 讲 syscall 机制时提过)。这层包装让 shell 这种用户程序写 `sys_open(path, 0)` 就像调普通函数,底下其实是特权级切换。

### shell 的 cat 与 ls

前面三步搭好了「系统能 open/read 文件」的能力,最后让用户敲得到。shell 加了两个命令,见 [cmd_cat.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/programs/shell/cmd_cat.cpp) 和 [cmd_ls.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/programs/shell/cmd_ls.cpp):

```cpp
// cat:打开文件,循环读,往 stdout 写
void cmd_cat(int argc, char** argv) {
    int64_t fd = sys_open(argv[1], 0);          // O_RDONLY
    if (fd < 0) { write_str("cat: cannot open ...\n"); return; }
    char buf[256];
    while (true) {
        int64_t n = sys_read(fd, buf, 256);
        if (n <= 0) break;                       // 读到尾(0)或出错(-1)
        sys_write(1, buf, n);                    // 写到 fd 1(stdout)
    }
    sys_close(fd);
}

// ls:打开目录,循环 getdents 取名字
void cmd_ls(int argc, char** argv) {
    const char* path = (argc >= 2) ? argv[1] : "/";
    int64_t fd = sys_open(path, 0);
    if (fd < 0) { /* 报错 */ return; }
    char name[256];
    while (true) {
        int64_t n = sys_getdents(fd, name, 256);
        if (n <= 0) break;
        sys_write(1, name, n);    // 名字
        sys_write(1, "\n", 1);    // 换行
    }
    sys_close(fd);
}
```

两个命令的模式一样:**open 拿 fd → 循环 read/getdents 到返回 ≤0 → close**。这是「读一个流」的标准骨架。`cat` 的 read 循环靠 sys_read 自动推进 offset,每次拿到下一块;`ls` 的 getdents 循环靠 offset 当下标自动推进,每次拿到下一个名字。两者都不用自己管「读到哪了」——VFS 层替它们管。这就是 003 那套「File 持有 offset」设计在用户态的回报:用户程序写得极简,状态全在内核。

## 调试现场
