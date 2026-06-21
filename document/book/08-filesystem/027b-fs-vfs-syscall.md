---
title: 027b · 让用户态能 open/read 文件:VFS 系统调用与 shell
---

# 027b · 让用户态能 open/read 文件:VFS 系统调用与 shell

> 027 在内核里搭好了整套 VFS:挂载表、inode、文件描述符表、ramdisk 后端。但那间房没开门——用户态的程序碰不到它,因为还没有「打开文件」「读文件」的系统调用。这一篇(027 的下半)就是给 VFS 装门:把 `open`/`read`/`write`/`close`/`getdents` 这些系统调用接到 VFS 管线上,再在用户态 libc 里包一层薄壳,最后让 shell 长出 `cat` 和 `ls` 两个命令。完成后,你就能在 shell 里敲 `cat hello.txt` 看到文件内容、敲 `ls` 列出目录——VFS 真正被人用上了。

## 这一章我们要点亮什么

把 027 那套「内核内部 VFS」暴露给用户态,形成一条完整的「用户程序 → 系统调用 → VFS → 文件」链路。三件事:

第一,**系统调用层**:实现 `sys_open`(路径解析 → inode → 分配 fd)、`sys_read`/`sys_write`(通过 fd 找 File、调 inode 的操作)、`sys_close`、`sys_getdents`(列目录),把它们接进 023 那套 syscall 派发表。

第二,**用户态封装**:libc 里给每个系统调用写一个薄薄的汇编包装(就是一句 `syscall` 指令加参数装载),让用户程序像调普通函数一样 `sys_open(...)`。

第三,**shell 落地**:`cat`(open + 循环 read + 往 stdout write)和 `ls`(open 目录 + 循环 getdents)两个命令,把前面两件事串成一个用户能敲的命令。

验收点很直观:进 shell 敲 `cat hello.txt`,看到 `Hello from Cinux!`;敲 `ls`,看到文件清单。这两条能跑,说明从用户键盘到 VFS 到 ramdisk 的整条链路通了。

## 为什么现在需要它

为什么紧接 027。027 把 VFS 装修好了,但 VFS 的入口在内核里,用户态够不着。系统调用就是那扇门——它是用户态进入内核态的唯一正门(023 已经搭好了 `syscall` 指令和派发框架)。这一篇要做的是:在这扇门后面接上通往 VFS 的走廊。

为什么要把 read/write 也改。023/024 那会儿,`sys_write` 是直接往串口/屏幕写的、`sys_read`(fd=0)是直接读键盘的——它们没经过任何「文件」抽象。现在有了 VFS,read/write 就该统一走「fd → File → inode 操作」这条路:你 write 到 fd 1(stdout)还是 write 到一个打开的普通文件,用的是同一套机制(只是底层 inode 的操作不同)。这一篇把 read/write 收编进 VFS 框架,顺带保留 fd 0(stdin)读键盘的老路(下面解释为什么)。

还有一笔关于「用户传进来的指针」的账。系统调用的参数里有用户态地址(比如 `open(path,...)` 的 path 指针、`read(fd,buf,...)` 的 buf 指针)。内核不能盲目信用户传的地址——用户可能传个空指针、或传个内核地址企图让内核替它读内核内存。所以每个收地址的系统调用,都得先做「这个地址合法吗」的检查。这一篇用 x86-64 的「规范地址」规则来卡这道关,是系统调用安全的基本功。

## 设计图

系统调用把用户态的「文件操作」翻译成 VFS 的「inode 操作」。看 open + read 这条主链:

```text
   用户态: sys_open("/hello.txt", O_RDONLY)
        │  (libc 包装:一句 syscall 指令,参数进 rdi/rsi/rdx)
        ▼  syscall 陷入内核 ── 派发表 ──▶ sys_open(path_virt, flags, ...)
   sys_open:
        ① 规范地址检查(path_virt 合法、非空、非内核态地址)
        ② fs = vfs_resolve(path, &rel_path)      ← 027 的挂载表
        ③ inode = fs->lookup(rel_path)            ← 后端按名找
        ④ fd = g_global_fd_table().alloc(inode, flags)   ← 分配描述符
        return fd  (≥3)  或 -1

   用户态: sys_read(fd, buf, 256)
        ▼  sys_read(fd, buf_virt, count):
        ① 规范地址检查(buf_virt)
        ② if fd==0: 走老路读键盘(stdin)        ← 保留的特殊路径
           else:
              file = FDTable.get(fd)
              n = file->inode->ops->read(inode, file->offset, buf, count)  ← 调到后端
              file->offset += n                              ← ★ VFS 层推进偏移
        return n
```

关键在两层分工:**系统调用层**负责「fd 状态 + 偏移推进 + 地址安全」这些和具体文件系统无关的事;**inode 操作层**(027 的 InodeOps)负责「从哪读、怎么读」这些后端特定的事。sys_read 调完 `ops->read` 后自己把 `file->offset` 加上读到的字节数——偏移是 VFS 层管的,后端的 read 只管「从 offset 处给我 count 字节」,不管「下次从哪接着读」。

`getdents`(列目录)复用了一个巧妙的设计:

```text
   sys_getdents(fd, buf, count):
        file = FDTable.get(fd)
        n = file->inode->ops->readdir(inode, file->offset, buf, count)   ← offset 当条目下标用
        if n==1: file->offset++ ; return 名字长度        ← 下标 +1,下次读下一条
        return n   (0 = 目录读完)
```

`file->offset` 在这里**兼当目录条目的下标**——read 用它当字节偏移,getdents 用它当条目序号。同一个字段两种语义,因为对目录而言「偏移」就是「第几个条目」。这样 shell 的 `ls` 只要循环 getdents 到返回 0,就能把目录列完。

## 代码路线

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

为什么 fd 0 要特殊处理?因为 stdin 不是「VFS 里的一个文件」——它没有 inode、不在挂载表里,FDTable 也不给 fd 0 分配 File(027 里 FDTable 从 fd 3 起算,0/1/2 是空的)。stdin 的数据来自键盘,所以 fd 0 的 read 直接走键盘驱动那条老路(014 写的 PS/2 键盘)。而 fd > 0 都是 VFS 里 open 出来的真实文件,走「File → inode 操作」。这两条路并存,是 stdin/stdout 这对「假文件」和真文件的本质差异。

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

这里 `file->offset` 当**目录条目的下标**用(027 的 `ramdisk_readdir` 里,index 0 是 `.`、1 是 `..`、2 起是文件条目)。每次 getdents 读一条、offset++,循环到返回 0 就列完了。返回值是「这次读到的名字长度」(不是字节数、也不是条目数),调用方拿这个长度去切 buf 里的名字。这种「一次一条、靠 offset 推进」的接口,是最朴素的 getdents 形态(POSIX 的 `getdents` 返回的是结构体数组、一次多条,这里简化成一次一条)。

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

两个命令的模式一样:**open 拿 fd → 循环 read/getdents 到返回 ≤0 → close**。这是「读一个流」的标准骨架。`cat` 的 read 循环靠 sys_read 自动推进 offset,每次拿到下一块;`ls` 的 getdents 循环靠 offset 当下标自动推进,每次拿到下一个名字。两者都不用自己管「读到哪了」——VFS 层替它们管。这就是 027 那套「File 持有 offset」设计在用户态的回报:用户程序写得极简,状态全在内核。

## 调试现场

027b 没有 notes,但系统调用接 VFS 这一路有几个高频坑。

一是 **规范地址检查漏掉或写错**。`sys_open`/`sys_read`/`sys_write`/`sys_getdents` 开头那几行 bit47 判定不能省。漏了,用户传个内核态地址(高位全 1),内核就 `reinterpret_cast` 去读那块内核内存——要么读到一堆内核数据泄给用户,要么触发缺页。写错(比如条件反了)会把合法的用户指针也拒掉,所有 open/read 都返回 -1。这道闸是「系统调用收用户指针」的标配。

二是 **fd 0 的 stdin 特殊路径忘了保留**。如果把 sys_read 写成「一律走 VFS」,fd 0 在 FDTable 里没有 File(027 从 fd 3 起分配),`get(0)` 返回 nullptr,read 直接 -1——于是 stdin 读不到任何键。stdin/stdout 是「假文件」,不在 VFS 里,fd 0/1/2 的读写得保留键盘/屏幕那条老路,只有 fd≥3 才走 VFS。

三是 **read 完没推进 offset**。漏了 `file->offset += result`,`cat` 会死循环打印文件开头那几字节,永远读不到结尾——因为每次 read 都从 offset 0 开始,readdir/offset 推进是 VFS 层的职责,后端 read 是无状态的。看到「文件内容只打印了第一块、反复刷」,先查 offset 推进。

四是 **getdents 的 offset 语义搞错**。对目录,`file->offset` 是条目**下标**不是字节偏移。如果忘了 `file->offset++`,`ls` 会反复 getdents 到同一条(永远是 `.`),死循环。如果误把 offset 当字节偏移传给 readdir,readdir 的 index 参数就乱了,列出来的名字错位。

五是 **write 到只读文件系统没处理 -1**。ramdisk 的 `ramdisk_write` 恒返回 -1(027 讲过)。如果将来有个 `echo > file` 之类的命令不检查 sys_write 返回值、以为写成功了,其实啥也没写进去。只读文件系统上的写操作就是失败,调用方必须看返回值。

## 验证

系统调用接 VFS 的行为,主要在 QEMU 里验(涉及真陷入、真 VFS)。机内测 [test_vfs_syscall.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_vfs_syscall.cpp) 是这一篇最厚的一块(近 700 行):从用户视角(或直接调 sys_open 等)测 open 找到文件、read 读出内容、close 释放、getdents 列目录、各种错误路径(文件不存在、fd 非法、表满)。FDTable 的分配回收逻辑在 host 上镜像测 [test_fd_table.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_fd_table.cpp)。

```bash
ctest --test-dir build -R 'fd_table' --output-on-failure      # host
cmake --build build --target run-kernel-test                  # QEMU 机内测
```

最直观的验收是跑起来进 shell 敲命令。`cat hello.txt` 应输出文件内容(`Hello from Cinux!`),`ls` 应列出 initrd 里的文件名。这两条命令能跑,说明「libc 包装 → syscall 陷入 → VFS 解析 → ramdisk 后端 → 数据回到用户态」整条链路通了。这一篇的难点和前几篇一样:正确性靠现象间接验证,所以机内系统调用测(焊死 open/read/close/getdents 的行为)+ 真 shell 跑一遍缺一不可。

## 下一站

到这里,从用户敲 `cat hello.txt` 到 ramdisk 里读出文件内容的整条路通了:系统调用把文件操作接进了 VFS,VFS 派发给 ramdisk 后端,数据回到用户态。但你会发现这套文件系统有个硬限制:它是**只读**的(ramdisk 的 write 恒 -1),而且数据是构建期嵌进内核的、不是从磁盘上读的。它证明「文件抽象」成立,但不是个能持久化、能写的真文件系统。

下一站(028),我们做真正的文件系统——ext2:它活在 025 那块 AHCI 磁盘上,能读也能写、有块分配、有目录层级。027/027b 这两篇搭的 VFS 抽象(inode、操作表、挂载、系统调用)正好是它的地基:ext2 只要实现 `FileSystem` 接口,就能挂进同一个 VFS,用户态的 `cat`/`ls` 一行不改就能用上它。不过那是下一章的事,我们先把「用户态能 open/read 文件」这个里程碑坐实。

---

### 参考

- Linux man-pages — [`open(2)`](https://man7.org/linux/man-pages/man2/open.2.html)、[`read(2)`](https://man7.org/linux/man-pages/man2/read.2.html)、[`getdents(2)`](https://man7.org/linux/man-pages/man2/getdents.2.html):系统调用语义对照。Cinux 这版是简化形态(如 getdents 一次一条、open 的 flags 只有 RDONLY/WRONLY/RDWR),POSIX 的完整语义(创建、权限、一次多条结构体)本章没实现,别拔高。
- Intel SDM Vol.1 — Canonical Address:x86-64 虚拟地址的「规范形」规则(bit 47 决定用户/内核半区,bit 48–63 必须与 bit 47 一致),这是 sys_open 等做地址合法性检查的硬件依据。本地 PDF `document/reference/intel/SDM-Vol3A-*.pdf`,可搜 "Canonical" 复核。
- 027 章 · [给文件一个统一接口:VFS 内核层](027-fs-vfs.md):这一篇接的就是 027 搭的 VFS 管线(inode、FileSystem、挂载表、FDTable)。两篇是同一个 tag 的上下半。
- 本 tag 源码:[sys_open.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/sys_open.cpp) / [sys_read.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/sys_read.cpp) / [sys_getdents.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/sys_getdents.cpp) / [sys_close.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/sys_close.cpp)、[syscall_nums.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/syscall_nums.hpp)、[syscall.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/libc/syscall.cpp)、[cmd_cat.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/programs/shell/cmd_cat.cpp) / [cmd_ls.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/programs/shell/cmd_ls.cpp);测试 [test_vfs_syscall.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_vfs_syscall.cpp)、[test_fd_table.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_fd_table.cpp)。
