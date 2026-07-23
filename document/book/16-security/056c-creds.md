---
title: 056c · 进程凭证:给每个任务记一笔「你是谁」,以及为什么 fork 自动就继承了
---

# 056c · 进程凭证:给每个任务记一笔「你是谁」,以及为什么 fork 自动就继承了

> 安全的另一条线是「身份」——得先知道每个进程「是谁」(哪个用户、哪个组),才能在后面(文件系统那一卷)做权限检查。这一章给每个任务加上凭证字段(uid/euid/gid/egid),再开六个查改它们的 syscall(getuid/setuid 那一族)。有意思的地方是继承:给 Task 加字段这种事,通常要同步改 fork/clone 的拷贝逻辑,但这里几乎零改动——因为 fork 是把整个 Task 结构体 `memcpy` 给子进程,新加的标量字段会**自动被拷过去**。这是一笔白捡的便宜,值得看清它为什么成立。诚实的边界先说在前头:做完这一步,**所有进程都还是 root(uid 0)**——内核里还没有「检查这个 uid 能不能访问这个文件」的逻辑(那是文件系统那一卷的事)。这一步交付的是凭证的**基础设施**(字段 + 查改 + 继承),不是真正的权限隔离。
>
> 所以这一章的验证偏 B 档:靠测试确认字段默认是 root、get 返回当前值、setuid 的简化规则生效(root 能设、非 root 只能退回 real)、fork 的 memcpy 真把凭证拷过去了——而不是「setuid 之后越权访问文件被拒」那种端到端现象(那要等文件权限检查落地)。

## 这章咱们要点亮什么

1. **四个字段为什么是两对**:uid/euid(真实 / 有效用户)、gid/egid(真实 / 有效组)——「你是谁」和「你当前以谁的身份行事」是两件事。
2. **fork 自动继承这笔便宜**:Task 整体 `memcpy`,标量字段不用单独拷;加字段 = 免费继承。
3. **setuid 的简化语义**:root(euid==0)能随便设,非 root 只能把自己的 effective 退回到 real——够覆盖「提权」和「降权」两类基本操作,复杂的(saved-set、setuid binary)留后面。
4. **诚实边界**:凭证基础设施有了,但还没人查它,所以全 root;真隔离在文件系统那一卷。

## 四个字段:real 和 effective 各一对

先看加在哪。`Task` 结构体里(`process.hpp`),凭证字段挂在 `controlling_tty` 后面:

```cpp
// process credentials -- real/effective user/group IDs. Default 0 = root.
// Inherited across fork()/clone() via their memcpy of the whole Task (the
// post-memcpy override sections do not touch these), so no explicit copy
// code is needed. setuid-binary support (execve honoring S_ISUID) and the
// saved-set ... deferred to F6 alongside file-permission enforcement.
uint32_t uid{0};   ///< Real user ID (0 = root)
uint32_t euid{0};  ///< Effective user ID (0 = root)
uint32_t gid{0};   ///< Real group ID (0 = root)
uint32_t egid{0};  ///< Effective group ID (0 = root)
```

（`process.hpp:256` 起,注释把「为什么不用单独写继承代码」直接写明了。）默认都是 0——0 是 root。内核的 init 任务是 root,它 fork 出来的 shell 也是 root,一路 root 到底(原因下面讲)。

为什么是两对、不是各一个?这是 Unix 的老设计:**real** 是「你登录时是谁」,一辈子基本不变;**effective** 是「你当前以谁的身份在行事」,会随 `setuid` 变。平时它俩相等;当一个普通用户去跑一个带 setuid 位的程序(比如 `passwd`),程序加载时内核会把 effective 临时提到文件属主(通常 root),干完活再退回去——real 还是那个普通用户,effective 临时是 root。权限检查看的是 effective(「你现在能干啥」),记账看的是 real(「这事最终算谁的」)。

Cinux 这一卷只把字段和基本的 `setuid` 立起来,**setuid binary**(execve 时根据文件的 setuid 位自动提/降 effective)那种自动化留到文件系统那一卷——因为它和文件权限是同一摊事。所以这里的 effective 主要靠手动 `setuid` 改。

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
