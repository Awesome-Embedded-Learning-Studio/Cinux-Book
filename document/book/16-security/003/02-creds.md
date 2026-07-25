---
title: 02 · fork 继承、setuid 与诚实的边界
---

# fork 继承、setuid 与诚实的边界

## fork 自动继承:一笔白捡的便宜

这一节是这一章最该看清的。给 Task 加字段,第一反应是「得去 fork/clone 里把新字段也拷一份」。但 Cinux 的 fork 不是逐字段拷,是**整个结构体一把 memcpy**:

```cpp
std::memcpy(child, parent, sizeof(Task));
```

（`fork.cpp:128`,clone 里同理。）这一把 `memcpy` 把父进程的**所有**字段——包括刚加的 uid/euid/gid/egid——原样复制到子进程。fork 拷完之后,只去覆盖那些**不该继承**的字段:tid/pid/tgid/ppid 这些(子进程得有自己的身份)。凭证字段不在覆盖清单里,所以它们保留父进程的值——**继承就自动完成了,零改 fork/clone 代码**。

这就是注释里那句「no explicit copy code is needed」的来历。它成立有两个前提,都值得记一笔:

- **字段是标量**(uint32_t,不是指针/不是持有资源的句柄)。标量 memcpy 就是值拷贝,父子各持一份,互不影响。要是指针(比如指向共享对象),memcpy 只拷了指针值,父子指向同一份,那就得单独处理共享语义——这正是 fork 里别的地方在忙活的事(地址空间、fd 表这些共享资源得用引用计数)。凭证是标量,躲开了这套麻烦。
- **字段位置不撞 context_switch.S 依赖的布局**。汇编的 `context_switch` 依赖 `ctx` 在 Task 偏移 0、`on_cpu` 在紧接 `CpuContext` 之后的位置(`process.hpp` 里有 `static_assert` 钉死)。凭证字段加在 `controlling_tty` 后面,不挪这些关键偏移,汇编不用动——`static_assert` 编译期就坐实了这点。

> 这条「memcpy 继承」的路径,在 `test_creds` 里还单独验了一次:测试自己 `memcpy` 一个 Task,然后断言四个凭证字段都拷过去了。这相当于把生产 fork/clone 的继承路径**单元测试化**——不真起 fork,只验「memcpy 确实带走了凭证」这件事本身。因为生产 fork 用的就是同一个 memcpy,这个单测等价于验了继承。

## setuid 的简化语义

六个 syscall(getuid/geteuid/getgid/getegid/setuid/setgid)里,前四个就是读字段返回,没花样。有内容的是 `setuid`(和它镜像的 `setgid`):

```cpp
// Simplified POSIX setuid: root (euid==0) sets euid freely; a non-root task
// may only drop euid back to its real uid. Returns 0 / -EPERM.
int64_t sys_setuid(uint64_t uid_arg, ...) {
    ...
    if (task->euid == 0 || new_uid == task->uid) {
        task->euid = new_uid;   // root 随便设;非 root 只能退回 real
        return 0;
    }
    return -kEperm;
}
```

（`sys_creds.cpp:41`,判断在 `:47`。）规则简化成两条:

- **root(euid==0)能设成任意 uid**——这是「提权」的入口(root 想扮成谁就扮成谁)。
- **非 root 只能把自己的 effective 退回到 real**(`new_uid == task->uid`)——这是「降权」:一个临时提到 root 的进程,干完活把自己的 effective 退回真实身份。

其余情况返 `-EPERM`(「operation not permitted」,`errno.hpp:22`,errno 1)。注意这里用的是 `kEperm`(EPERM,操作不允许),不是 `kEacces`(EACCES,文件权限拒绝)——前者是「你没资格做这个操作」(setuid 失败),后者是「你对这个文件没权限」(留到文件系统那一卷)。两个 errno 别混。

完整的 POSIX setuid 规则要复杂得多(有 saved-set uid、有「非 root 能不能在 real/effective/saved 之间来回切」的细致条款),这里只取了「提权 + 降权」这两条最基本的,够撑起后续的权限模型骨架。复杂的 saved-set 留文件系统那一卷(它和 setuid binary 是同一摊)。

六个 syscall 用的是 Linux x86_64 的标准号(`syscall_nums.hpp`):`getuid=102`、`getgid=104`、`setuid=105`、`setgid=106`、`geteuid=107`、`getegid=108`。对齐标准号是为了将来跑 Linux 程序(用户态那一卷的 musl/ELF)时号能对上。

## 诚实的边界

**做完这一步,所有进程都还是 root。** 凭证字段有了、能查能改了、fork 也继承了——但内核里**没有任何地方在检查凭证**。没有 `check_permission(inode, task)` 这种调用,文件系统的 inode 虽然带着 uid/gid/mode(ext2 存的,`stat` 也返 `st_uid`/`st_gid`),但没人拿它和当前进程的 euid 比对。所以 init 是 root、fork 出的 shell 是 root、shell 起的程序还是 root——`setuid` 能把 effective 改成别的值,但改了也没东西在乎。**这一步交付的是凭证的「记号笔」(字段 + 查改 + 继承),不是「门禁」(权限检查)**。门禁是文件系统那一卷的事。

**没有 setuid binary。** execve 加载程序时,没有去看可执行文件的 setuid 位(那个位会让程序以文件属主身份运行,是 `passwd` 之类工具的原理)。所以「跑一个程序自动提/降权」这条自动化链路没接上,得手动 `setuid`。同样留文件系统那一卷(和 `check_permission` 同域)。

**`setuid` 的规则是简化版。** 只有「root 全能 + 非 root 退回 real」两条,没有 saved-set uid(那个能让你在 real 和 effective 之间反复横跳)。对教学内核够用,真要对齐 POSIX 得补 saved-set。

验证该看到什么,见配套 lab。到这儿,F9 安全这一卷的三块(NX/SMEP/SMAP、ASLR、凭证)就齐了——硬件隔离、布局随机化、身份记录。下一卷该换条线了。
