---
title: 01 · 导引:点亮什么、为什么、设计图
---

# 导引:点亮什么、为什么、设计图

> 到 033 为止,Cinux 已经有了一个像样的桌面:画布、窗口管理器、桌面图标、还有一个能跑 shell 的终端窗口。但整个系统本质上还是「一个进程跑到底」——开机时 init 线程把一切搭起来,shell 在终端里转,却没有任何办法在运行时**再生出一个进程**。你不能 fork 出一个子进程去干活,也不能让一个进程 execve 成另一个程序,更没法等它结束、把它的尸体收掉。这一章,我们把 Unix 进程模型的三大原语——`fork`(生)、`execve`(换)、`waitpid`(收)——一次性接进内核。做完这步,Cinux 才真正有了「多进程」的底子。

## 这一章我们要点亮什么

一件最能说明问题的事:一个进程调用 `fork`,自己继续往下跑(拿到子进程的 PID),与此同时一个**几乎一模一样**的子进程被造出来、被调度器排上队;子进程之后可以 `execve` 成磁盘上的另一个 ELF 程序,把整个内存映像换掉但 PID 不变;父进程用 `waitpid` 等子进程退出、拿到它的退出码,再把它的 TCB 清掉。

这一套背后,点亮了五样新东西:

- **五个新系统调用**:`getpid`(39)、`getppid`(110)、`fork`(57)、`execve`(59)、`waitpid`(61)。号码**完全沿用 Linux x86_64**——这不是巧合,是有意为之,方便以后把用户态程序往 Linux 靠。
- **第一个 PID 分配器**。进程得有自己的身份证号,而且号会回收(子进程死了,号得能再分给别人)。
- **TCB 的「家谱」字段**。`Task` 多了 `pid`/`ppid`/`exit_status`/`children`/`parent`,以及一个新的生命周期状态 `Zombie`。没有这些,父子关系和收尸都无从谈起。
- **Copy-On-Write 页表**。fork 不是傻乎乎把父进程所有内存抄一遍,而是让父子**共享**物理页、写时再复制。这是这一章最有分量的设计。
- **ELF 加载器**。`execve` 要从 VFS 里把一个 ELF 程序读出来、校验、铺进地址空间、把入口写进上下文。这是 022 那次「进用户态」的升级版——那次是内核硬塞一个固定的 shell,这次是**任意**磁盘上的 ELF。

> 一句话:022 让我们**能**进用户态跑程序,034 让我们**能在运行时换程序、生程序、收程序**。

## 为什么现在需要它

回看 033 留下的局面:GUI 桌面跑起来了,终端里有个 shell,但这个 shell 是开机时由 init 线程**一次性**带起来的,全系统就这一个用户进程。你想再开一个终端、让里面跑**另一个独立的** shell?做不到——没有 fork,「生一个新进程」这件事压根不存在。你想让一个进程临时换去跑磁盘上的某个工具再换回来?也做不到——没有 execve。

所以 034 要补的是三件事的**原语**:

```text
生(fork):  当前进程 → 复制出一个子进程(几乎相同的内存、相同的代码位置)
换(execve):当前进程 → 丢掉整个内存映像,换成磁盘上某个 ELF,PID 不变
收(waitpid):父进程 → 等某个子进程退出,拿退出码,清掉它的 TCB
```

这三件事合起来,才让「开一个终端、里面 spawn 一个独立 shell 进程」成为可能——而那正是下一章(035 多终端)要干的事。所以这一章是 035 的地基:先把进程的生老病死打通。

顺带,`paging_config.hpp` 这一章只改了**一行**——加了个 `FLAG_COW = 1 << 9`。就这一行,撑起了整个 Copy-On-Write 机制。别小看它。

## 设计图

先看 fork 把一个进程「生」出来的全链路:

```text
                      父进程 TCB(parent)
                            │  Scheduler::current()
                            ▼
                   ┌────────────────────┐
            ① 分配 child_pid ◀──────────│ PidAllocator::alloc()
                   └─────────┬──────────┘
                             │
                   ┌─────────▼──────────┐
            ② new Task; memcpy(child,parent,sizeof(Task))
                   │  整个 TCB 原样拷贝    │
                   └─────────┬──────────┘
                             │
                   ┌─────────▼──────────┐
            ③ 修字段:tid/pid/ppid/state=Ready/parent/children=nullptr
                   └─────────┬──────────┘
                             │
                   ┌─────────▼──────────┐
            ④ 新内核栈(16KB) + 拷贝父栈「已用区」+ 写 STACK_MAGIC
                   └─────────┬──────────┘
                             │
                   ┌─────────▼──────────┐
            ⑤ CoW 页表:子进程拿全新 AddressSpace
                   │  递归 3 层,叶子层共享物理页 + 双方改 RO|COW
                   └─────────┬──────────┘
                             │
                   ┌─────────▼──────────┐
            ⑥ 挂进 parent->children 链;Scheduler::add_task(child)
                   └─────────┬──────────┘
                             │
                       return child_pid   ◀── 父进程拿到这个
```

注意最后那行:fork 是**父进程视角**的返回。子进程怎么「返回」、返回什么,是个微妙的问题——我们留到「调试现场」专门讲,因为 034 在这里的处理和你直觉上的「教科书 fork」**并不一样**。

再看 Copy-On-Write 的核心——父子共享同一张物理页,谁写谁复制:

```text
  fork 前(父进程一片可写用户页):
    父PTE:  phys=0x1000 | PRESENT | WRITABLE | USER

  fork 后(copy_page_table_level 叶子层,把父子双方都改了):
    父PTE:  phys=0x1000 | PRESENT | USER | COW        ← 去掉 WRITABLE,置 COW
    子PTE:  phys=0x1000 | PRESENT | USER | COW        ← 指向同一物理页!

  〔设计意图〕父或子任何一方写入 0x...00 ──► #PF(写只读页) ──► handle_cow_fault:
        1. alloc 新物理页 0x9000
        2. 把 0x1000 的 4096 字节拷到 0x9000
        3. 把「写的那一方」的 PTE 改成 phys=0x9000 | PRESENT | WRITABLE | USER(清 COW)
        4. invlpg 刷 TLB
        ──► 写的那一方从此用私有页 0x9000;另一方仍指 0x1000,互不干扰

  ⚠ 但 034 里这条 #PF → handle_cow_fault 的路径【还没接上】:handle_pf 只做
     demand-paging(present=0 补页),写保护故障(present=1)直接 fatal_halt。
     handle_cow_fault 写好了却没人调用——真写一张 CoW 页会停机,要等后面接进 #PF。

  〔后续注〕这条 CoW 路径此后已接通:当前源码 page_fault.cpp 的 handle_pf 里
     已加入 if (cinux::proc::handle_cow_fault(fault_addr)) 短路返回。本节其余
     描述以 034 tag 当时状态为准。
```

execve 的流程是一条「拆旧建新」的流水线:

```text
  execve("/path/to/prog")
        │
        ▼
  vfs_resolve → fs->lookup ──► inode  (找不到 → ENOENT)
        │  inode->type != Regular? ──► EISDIR
        ▼
  读 64 字节 ELF 头 → validate_elf_header  (魔数/类别/字节序/机型/类型)
        │
        ▼
  读 program headers(phnum × 56 字节)
        │
        ▼
  clear_user_mappings:  把旧的用户空间页(数据页 + PT/PD/PDPT 页)全释放
        │
        ▼
  遍历每个 PT_LOAD 段:
    对 [p_vaddr, p_vaddr+p_memsz) 里每一页:
      alloc 物理页 → 整页清零 → 按 p_filesz 从 inode 拷文件字节
                                     (p_memsz > p_filesz 的尾部天然是零 = BSS)
      → addr_space->map(vaddr, phys, flags)
        │
        ▼
  task->ctx.rip = ehdr->e_entry    ◀── 入口写进上下文,新映像就位
```

最后是 waitpid 的「收尸」,在父进程的 children 单链表里找 zombie:

```text
  parent->children ──► [child A:Ready] ──► [child B:Zombie,exit=7] ──► [child C:Ready]
                                                    ▲
                                    waitpid(-1) 扫到第一个 Zombie = B
                                                    │
                          收 *status = 7 → 从链表摘掉 B → pid_alloc.free(B.pid) → B.state = Dead
```
