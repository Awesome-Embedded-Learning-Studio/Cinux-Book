---
title: 046 · clone / futex / TLS
---

# 046 · 从"复制进程"到"按需共享":clone、futex 与线程

> `fork` 造的是**进程**——整个地址空间、文件描述符表、信号处理表,统统复制一份(地址空间还是写时复制的懒复制,但逻辑上是"新的一套")。可很多时候你要的不是"全新一套",而是**线程**:共享同一块地址空间、同一套文件描述符,只是各自有独立的执行流和栈。Linux 的 `clone` 就是干这个的——它按一组 flag 决定"哪些共享、哪些复制",共享到极致就是线程。这一章实现 `clone`,配上 `futex`(线程怎么同步)、TLS(每线程的局部存储)、cleartid(`pthread_join` 的底),把 POSIX 线程的内核基础铺好。

## clone:按 flag 共享或复制

`clone` 和 `fork` 的区别全在一组 `CLONE_*` flag:带哪个 flag,对应资源就**共享指针**(指向同一份);不带,就**复制一份**(fork 语义)。

- `CLONE_VM` → 共享地址空间。**这就是线程**——两个执行流跑在同一套页表上;
- `CLONE_FILES` → 共享文件描述符表;`CLONE_SIGHAND` → 共享信号处理表;`CLONE_FS` → 共享当前工作目录;
- `CLONE_THREAD` → 兄弟关系:新线程和调用者**同属一个线程组**(同 `tgid`),`getpid` 都返组长的 pid。

```cpp
// kernel/syscall/sys_clone.hpp —— Linux 风格 clone
int64_t sys_clone(uint64_t flags, uint64_t stack, uint64_t parent_tid, uint64_t child_tid, ...);
```

> 能这么"按 flag共享指针",有赖于一个前置:这些资源(地址空间、fd 表、信号表、cwd)从"深拷贝"改成了**带引用计数的共享对象**。共享指针时 bump 引用计数,析构时减。否则共享指向的东西会被一边提前释放——这是 clone 能落地的前提。

## TLS:每个线程自己的小块地盘

线程共享地址空间,但每个线程得有**只属于自己的存储**(比如 `errno` 的位置、线程局部变量)。这就是 TLS(Thread-Local Storage)。x86-64 上,`%fs` 段寄存器指向当前线程的 TLS 区——每个线程的 `%fs` 基址不同。

`clone` 带 `CLONE_SETTLS` 时,把子线程的 `fs_base` 设成调用者给的 TLS 地址;上下文切换切入这个线程时,恢复它的 `fs_base`。于是每线程访问"自己的 `%fs` 段",拿到的是各自的 TLS。

## futex:线程同步的底

线程共享内存,就需要**同步**——锁、条件变量。`futex`(fast userspace mutex)是 Linux 的同步原语底座:

```cpp
// kernel/syscall/sys_futex.hpp
int64_t sys_futex(uint64_t uaddr, uint64_t op, uint64_t val, ...);
```

思路是"用户态快速路径 + 内核慢路径":锁没竞争时,纯用户态原子操作搞定(不进内核);冲突了才 `futex` 系统调用——`FUTEX_WAIT` 在 `*uaddr==val` 时把当前线程挂起、`FUTEX_WAKE` 唤醒等者。内核侧维护一个"按 uaddr(用户地址)的等待队列"。pthread 的 mutex / condvar 都建在它上面。

## cleartid:`pthread_join` 的内核侧

线程要能被"等"——`pthread_join`。怎么实现的?`clone` 带 `CLONE_CHILD_CLEARTID` 时,记一个用户地址(`child_tid`)。线程退出时(`task_exit_cleartid`),把这个地址**清零 + `futex_wake` 一个等者**。而 `pthread_join` 就是在这个地址上 `futex_wait`。于是被 join 的线程一退出、清零、唤醒,join 的线程就被唤醒——**零额外机制,完全复用 futex**。这就是 Linux `pthread_join` 的内核侧协议。

## 一个不简单的点:新线程的用户栈

`fork` 的子进程返回**父栈**(写时复制共享)。可 `clone` 的子线程要返回**调用者给的栈**(线程有自己的栈),不能返回父栈。怎么做到?

系统调用入口建的寄存器帧(pt_regs)固定在内核栈顶的一个位置,其中 `user_rsp`(用户栈指针)在帧的 offset 0。clone 复用 fork 的"拷贝父内核栈"机制(子内核栈是父栈的副本,含这个帧),然后**直接改子帧的 `user_rsp` 槽**:

```cpp
if (stack != 0)
    *(uint64_t*)(child->kernel_stack_top - 96) = stack;   // 帧在栈顶固定位置
```

子线程经返回路径回到用户态时,`user_rsp` 就成了调用者给的栈、`user_rip` 是父的(共享代码)、`rax=0`(线程组里子线程 clone 返 0)。**帧在栈顶固定位置,直接按 `kernel_stack_top` 定位,不用从当前栈指针算偏移。**

## 验证

```bash
grep -rn 'sys_clone\|sys_futex' kernel/syscall/
grep -rn 'fs_base\|task_exit_cleartid\|CLONE_SETTLS\|CLONE_CHILD_CLEARTID\|CLONE_VM' kernel/proc/process.hpp kernel/proc/fork.cpp
```

构建:

```bash
cmake -B build -S . && cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
```

端到端:内核测试里 `clone(CLONE_VM|CLONE_THREAD|CLONE_SETTLS, stack, ...)` 起兄弟线程——两个线程同属一个线程组(同 `tgid`)、各跑各的 TLS、用 futex 协调。真用户态的 pthread 程序要等后面 musl 的 libpthread;但内核侧的线程原语,这一章就齐了。想体会 futex 的快速路径:无竞争时拿锁、放锁全在用户态原子指令完成,一次系统调用都不用——只有真冲突才进内核。
