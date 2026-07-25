---
title: 01 · 导引:不是搬字节,是搬地址
---

# 导引:不是搬字节,是搬地址

> IPC 四件套走到这一章,前面 pipe、FIFO 已经把「字节流过内核缓冲」这条路趟通——写者一次 syscall 把字节拷进管子、读者再一次 syscall 拷出来,两次穿越。这一刀(SysV 共享内存,shm)更狠:**它根本不搬字节,它搬地址**。两个进程各自 shmat 一块登记好的物理页到自己地址空间,底下落到的是**同一批物理帧**——写者写一个字节、读者**无需任何 syscall** 就在自己的虚拟地址里看到新值。零拷贝、零 syscall,这就是「共享」的真义。
>
> 可这套机制藏着一颗定时炸弹:进程退出时,地址空间析构要遍历每个用户 PTE 把映射的物理页「还回去」。如果一个共享段还活着、别的进程还在用,析构绝不能把段里的页误放回 free pool——否则就是 use-after-free 的物理版。这条账怎么记平,是这章最值得拆开看的一刀。再补一个 shmdt 长度的真实踩坑(取段的 `page_count`、不是取合并后 VMA 的跨度),shm 的正确性骨架就齐了。
>
> A 档:punchline 是**两个地址空间真把同一物理页映射通**——一个 AS 写 magic、另一个 AS 读回来完全相等,外加 translate 验证两虚拟地址确实落到同一物理帧。6 例 ring0 端到端测兜底。诚实的边界先摆前头:这套测用「栈上 AddressSpace + 空壳 Task」模拟两进程,不是真用户态 libc 跑通;`ShmRegistry` 设计上可链 host 单测但目前没配(对照 FIFO 有);shmid_ds 是精简内核内形状,跟 glibc 互操作还差一截。

## 这章咱们要点亮什么

1. **价值先说清:不是搬字节,是搬地址**。pipe 是字节流过内核 buffer(两次 copy),shm 是页表直接共享物理页(零 copy + 无 syscall 可见)。你一眼分清「搬数据 vs 搬地址」,后面 mapcount 闭环才有动机。
2. **分层铁律:纯逻辑表 vs 物理页生命周期**。`ShmRegistry` 是固定 16 槽的纯逻辑表(key→segment,只管簿记 + nattach/marked 状态机,零 kernel-only 依赖),`sys_shm` 才是物理页生命周期层(alloc_pages/map/unmap/free_pages)。这跟 071 的 `FifoRegistry` 是一个模子,跟 081 tmpfs「纯逻辑层 vs boot I/O 层」是同一套切法。
3. **双计数器闭环:段自带 refcount 基线 1 防 teardown 误放**。一块共享物理页的生命计数要保证进程退出绝不把「段还在用」的页误放回 free pool。Book 源码里是 `pte_count`(多少用户 PTE 映射此页)+ `refcount`(所有权引用)双计数器,alloc 给段 refcount 基线 1、shmat 同时 inc 两者、teardown 走两级 test。
4. **shmdt 长度陷阱:取段 page_count,不取合并后 VMA 跨度**。两段背靠背映射会并成一个 VMA,shmdt 按 VMA 算长度会顺带拆邻居的页——必须用段的 `page_count` 做权威长度。
5. **诚实分层的好处:ShmRegistry 设计上可链 host 单测,目前是缺口**。这层纯逻辑零 kernel-only 依赖,设计上 host 可测(同 fifo.cpp);但当前 test_shm 6 例全走 syscall 层在 ring0 跑,registry 的纯逻辑只被顺带覆盖——对照 FIFO 有 `test_fifo.cpp`,shm 没配,这是对称缺口不是 bug。

## 两进程共享一页要解决什么:不是搬字节,是搬地址

先回顾 071 的 pipe 和 FIFO。它们的本质是「**内核 buffer 搬字节**」:写者 `sys_write` 一次 syscall,把用户态字节拷进内核管道缓冲;读者 `sys_read` 再一次 syscall,把字节从内核缓冲拷出来。两次用户态↔内核态 crossing,两次 memcpy。这是字节流的代价。

shm 这一刀完全不同。`shm.hpp` 的头注释把机制一句话讲透了:

```cpp
/**
 * Two or more processes share a physical page range: shmget() allocates a
 * contiguous run of physical pages and registers it under an int key; shmat()
 * maps that run into the caller's address space; shmdt() tears the mapping
 * down; shmctl(IPC_RMID) marks the segment for destruction (the pages are freed
 * once the last attachment goes away).
 */
```

([shm.hpp](../../../kernel/ipc/shm.hpp#L5-L9),有删节。)

四个 syscall 拆开看:

- `shmget(key, size, shmflg)`——申请一块连续物理页,登记到一个 int key 下,返回 shmid(表索引)。
- `shmat(shmid, addr, shmflg)`——把这块物理页映射进**调用方**的地址空间,返回虚拟地址。多个进程对同一 shmid 各自调 shmat,各自拿到的虚拟地址**可以不同**,但底下的物理帧是**同一批**。
- `shmdt(addr)`——拆掉自己地址空间里这块映射(物理页不动,别的进程还共享着)。
- `shmctl(shmid, IPC_RMID, ...)`——标记销毁(段页最后一次 detach 后才真回收)。

这套机制的价值一句话说清:**pipe 搬数据,shm 搬地址**。写者写完一个字节,读者在自己进程里读那个虚拟地址——**没有任何 syscall**——就拿到新值。这是零拷贝 + 零 syscall 可见的「真共享」,代价是没有任何同步(写者和读者谁先谁后,得自己用信号量/互斥锁协调,不在 IPC 这层管)。

这四个 syscall 在 Book 里是真注册的,不是 stub。看 `syscall.cpp`:

```cpp
syscall_register(SyscallNr::SYS_shmget, sys_shmget);
syscall_register(SyscallNr::SYS_shmat, sys_shmat);
syscall_register(SyscallNr::SYS_shmctl, sys_shmctl);
syscall_register(SyscallNr::SYS_shmdt, sys_shmdt);
```

([syscall.cpp](../../../kernel/arch/x86_64/syscall.cpp#L224-L227)。)syscall 号落在 `syscall_nums.hpp`:`SYS_shmget=29`、`SYS_shmat=30`、`SYS_shmctl=31`、`SYS_shmdt=67`(`syscall_nums.hpp:46-48` 和 `:64`)——这四个号跟 Linux x86_64 ABI 对齐,musl/glibc 直接能调到。

> 串一句 071:那卷的 pipe/FIFO 搬数据,这卷的 shm 搬地址,是同一条 IPC 路上的姊妹刀。071 的 `FifoRegistry`(名字→FIFO)跟这卷的 `ShmRegistry`(key→segment)是直系模子——固定 16 槽 + index-as-handle,下一节展开。
