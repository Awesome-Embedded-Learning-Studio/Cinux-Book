---
title: 01 · 导引与四个字段
---

# 导引与四个字段

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
