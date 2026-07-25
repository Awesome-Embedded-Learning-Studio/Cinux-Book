---
title: Lab 003 · 进程凭证(uid/gid)验证
---

# Lab 003 · 进程凭证(uid/gid)验证

> 对应 `document/book/16-security/003/`。验证档偏 **B 档**:本章交付的是凭证**基础设施**(字段 + 查改 syscall + fork 继承),不是权限隔离——内核还没人查凭证,所以验不了「setuid 后越权访问文件被拒」那种端到端现象(那要等文件系统的权限检查落地)。验证靠 `test_creds` 单测 + grep:字段默认 root、get 返回当前值、setuid 简化规则生效(root 能设 / 非 root 退回 real)、fork 的 memcpy 真把凭证拷过去。

## 目标

确认四件事:

1. `Task` 加了 uid/euid/gid/egid 四字段(默认 0=root),挂在 `controlling_tty` 后(不撞 context_switch 偏移);
2. 六个 syscall(getuid/geteuid/getgid/getegid/setuid/setgid)挂上,用的是 Linux x86_64 标准号(102-108);
3. `setuid` 简化规则对:root(euid==0)能设任意、非 root 只能退回 real,否则 `-EPERM`;
4. fork 继承成立——`memcpy(child, parent, sizeof(Task))` 把四字段原样拷过去(`test_creds` 单测化的继承路径)。

## 步骤

### 1. Task 的四个凭证字段

```bash
sed -n '253,260p' kernel/proc/process.hpp
```

应看到 `uint32_t uid{0}` / `euid{0}` / `gid{0}` / `egid{0}`(默认 0=root),挂在 `controlling_tty` 后。注释会写明「fork/clone 的 whole-Task memcpy 自动继承,不用单独拷贝」——这就是下面要验的「免费继承」。字段加在 `controlling_tty` 后面是刻意的:不挪 `ctx`(偏移 0)和 `on_cpu` 这些 `context_switch.S` 钉死的偏移,汇编不用动。

### 2. 六个 syscall + 标准号

```bash
grep -n 'getuid\|getgid\|setuid\|setgid\|geteuid\|getegid' kernel/syscall/syscall_nums.hpp
```

应看到六个号:`getuid=102`、`getgid=104`、`setuid=105`、`setgid=106`、`geteuid=107`、`getegid=108`(Linux x86_64 标准号)。对齐标准号是为了将来跑 Linux 程序(musl/ELF)时 syscall 号能对上。

### 3. setuid 简化规则(B 档核心)

```bash
sed -n '41,52p' kernel/syscall/sys_creds.cpp
```

应看到 `sys_setuid` 的核心判断:`if (task->euid == 0 || new_uid == task->uid)` 才设成功,否则返 `-kEperm`。两条规则:root(euid==0)随便设;非 root 只能退回自己的 real(`new_uid == uid`)。注意用的是 `kEperm`(errno 1,EPERM,操作不允许),不是 `kEacces`(errno 13,EACCES,文件权限拒绝)——后者是文件系统那卷的事。`test_creds` 里会跑「root 全能 / 非 root EPERM + 退回 real」两组用例:

```bash
cmake --build build --target run-kernel-test 2>&1 | grep -iE "creds" | head
```

应看到 test_creds 的几项全 **PASS**。

### 4. fork 继承(memcpy 那笔便宜)

这是本章的明星点,值得单独看一眼测试怎么验的:

```bash
sed -n '97,112p' kernel/test/test_creds.cpp
```

应看到 `test_fork_inherits_via_memcpy`:测试自己 `__builtin_memcpy(&child, &parent, sizeof(Task))`,然后断言四个凭证字段都从 parent 拷到了 child。这相当于把生产 fork/clone 的继承路径**单元测试化**——不真起 fork,只验「memcpy 确实带走了凭证」。生产 fork(`fork.cpp:128` 的 `std::memcpy(child, parent, sizeof(Task))`)用的就是同一个 memcpy,所以这个单测等价于验了继承。

```bash
# 确认生产 fork 确实是整体 memcpy
grep -n 'memcpy(child, parent, sizeof(Task))' kernel/proc/fork.cpp
```

应命中 `fork.cpp:128`。clone 里同理(也走整体 memcpy)。

## 验收清单

- [ ] `process.hpp:256` 起 uid/euid/gid/egid 四字段(默认 0=root),挂 `controlling_tty` 后。
- [ ] 六 syscall 号 102/104/105/106/107/108 对齐 Linux x86_64 标准。
- [ ] `sys_setuid` 规则:`euid==0 || new==uid` 才设,否则 `-kEperm`;test_creds 全 PASS。
- [ ] `test_fork_inherits_via_memcpy` PASS,生产 `fork.cpp:128` 确实是整体 memcpy——继承路径验过。
- [ ] 知道**全进程仍 root**(没 check_permission,凭证字段还没人查)——这是基础设施,不是隔离。

## 别做这些

- **别**指望验「setuid 后越权访问文件收 EACCES」——内核还没 `check_permission`,所有进程都是 root,文件 inode 的 uid/gid/mode 没人比对。真权限隔离在文件系统那卷。
- **别**以为加了凭证字段就要改 fork/clone——整体 `memcpy(child, parent, sizeof(Task))` 自动把标量字段拷过去,凭证是标量、不撞 asm 偏移,零改继承代码。这是「免费午餐」,但只在字段是标量 + 位置不撞 asm 钉死偏移时成立。
- **别**把 `kEperm`(EPERM,setuid 这种「操作不允许」)和 `kEacces`(EACCES,文件「权限拒绝」)混用——前者是凭证/syscall 域,后者是文件权限域。
- **别**以为 setuid 规则完整——只有「root 全能 + 非 root 退回 real」两条,没有 saved-set uid、没有 setuid binary(execve 不查 S_ISUID 位)。真要对齐 POSIX 得补这俩。
