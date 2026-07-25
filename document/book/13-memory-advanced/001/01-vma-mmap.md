---
title: 01 · 给地址空间一本账:VMA 区域记账与 mmap
---

# 给地址空间一本账:VMA 区域记账与 mmap

> 你的用户程序想动态要一块内存,调 `mmap`。内核得回答一连串问题:这块虚拟地址归谁管?是匿名的还是映射到某个文件?可读可写可执行吗?它是栈、是堆、还是普通的映射?
>
> 问题是,040 之前内核的 `AddressSpace` 只有**页表**。页表回答"这个虚拟页映射到哪个物理页、什么权限",但回答不了"**这段虚拟地址是什么**"。于是每个想用虚拟内存的机制(mmap、堆扩展、共享内存、按需读文件)都得自己想办法记"自己管着哪段地址",口径不一、互相打架。这一章给 `AddressSpace` 配一本**区域账本**——VMA(Virtual Memory Area),让所有这些机制都来问它、改它,再在账本上实现 `mmap`。

## VMA:一段虚拟地址是什么

一条 VMA 记录一段虚拟地址 `[start, end)`:它的权限(读/写/执行)、它的属性(匿名 / 文件映射 / 栈 / 堆)、它背后挂的 inode(文件映射时)。`AddressSpace` 持有一个 VMA 的集合——一本"区域账本"。

账本怎么存?`kernel/mm/vma.hpp` 用一个按起始地址有序的链表(`LinkedListVMAStore`),两个关键操作:

- **插入时合并**:新加一段区域,如果和相邻区域权限相同、地址连续,就**合成一个**大区域——不让账本碎片化;
- **删除时拆分**:在一段中间取消映射(比如 `munmap` 掉一大段里的一小块),要把原区域**拆成两半**。

留了个抽象接口 `IVMAStore`——现在用链表(简单、区域少时够快),将来区域多了想换红黑树,只换实现、不动调用方。**抽象的时机是"第二个实现的真实需求出现",不是"看起来能抽象"——现在只有一个实现,接口先占着位。**

```cpp
// kernel/mm/vma.hpp —— 区域属性(位标志)
enum class VmaFlags : uint64_t { None, Read, Write, Exec, Shared, Anonymous, Stack, Heap, ... };
// kernel/mm/address_space.hpp —— 账本是 AddressSpace 的直接成员
LinkedListVMAStore vma_store_;
IVMAStore& vmas() { return vma_store_; }   // 访问账本
```

哪些地方往账本里记账?execve 加载 ELF 时,每个段记一条(按可写/可执行转 `VmaFlags`);建用户栈时,记一条带 `Stack` 标志的(`kernel/proc/user_launch.cpp`)——栈标志是给后面按需分页用的,栈可以向下长。

## mmap:只记账,先不给物理页

账本有了,`mmap` 就建在它上面。一个关键设计:**懒分配**。`sys_mmap` 只在账本里 `insert` 一条 VMA,**不映射任何物理页**。程序第一次访问这块地址时,触发 page fault,内核这时才真给它分配一页(按需分配,demand paging)。

```cpp
// kernel/syscall/sys_mmap.hpp —— mmap/munmap/mprotect 三个 syscall(都是 6 参,匹配 syscall 表)
int64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t offset);
int64_t sys_munmap(...);
int64_t sys_mprotect(...);
```

- **mmap**:`find_free_area`(或 `MAP_FIXED`)+ `insert` VMA。匿名映射记 `Anonymous`,文件映射记 backing inode(文件内容怎么读,下一章的 Page Cache 管);
- **munmap**:把范围内的页拆掉释放,再从账本 `remove`(可能拆分);
- **mprotect**:找到覆盖的 VMA,换掉它的读写执行权限,再对已经映射的页重新下发页表权限。

> 三个 syscall 都声明成 6 参,哪怕 `munmap` 只用前两个——因为它们要注册进统一的 syscall 表,类型必须匹配。后面几个参数是哑的。

还有一条:**fork 现在会复制 VMA**。fork 把页表写时复制一份之后,把父进程的 VMA 也逐条 insert 到子进程——这样子进程继承父的内存布局。这是 fork/exec 能正常工作的前提。

## 一个故意的妥协:page fault 先只诊断,不门控

有了账本,本来顺理成章该做一件 Linux 那样的事:page fault 时查 VMA,**没命中的就直接 segfault**(访问没登记的区域 = 野指针 = 杀进程)。但这一章**没这么做**,只做了**诊断**:fault 时查一下,没命中就 `klog_warn` 一声,但**仍然照常给页**。

为什么?因为现有的按需分配对任何不存在的地址都宽松映射(野指针、栈下方都给页)。想改成"没 VMA 就真 segfault",得**重构整个按需分配逻辑**(VMA 硬门控 + 栈自动下扩 + guard 页),漏判一处就是 shell 直接挂,高风险。所以这一章选了零风险的诊断路径,真 segfault 留到后面按需分配那一章。

> 教训:重构要分阶段。"有账本了就该立刻严格门控"是直觉,但严格门控依赖按需分配本身先改好。先把账本立起来当诊断用,等按需分配重构时再拧紧——比一次性大爆炸稳。

## 验证

```bash
# VMA 账本 + AddressSpace 集成
grep -rn 'class LinkedListVMAStore\|class IVMAStore\|enum class VmaFlags' kernel/mm/vma.hpp
grep -n 'vma_store_\|vmas()' kernel/mm/address_space.hpp
# 三个 mmap syscall
grep -rn 'int64_t sys_mmap\|int64_t sys_munmap\|int64_t sys_mprotect' kernel/syscall/
# 栈 VMA 注册(带 Stack 标志)
grep -n 'VmaFlags::Stack\|vmas().insert' kernel/proc/user_launch.cpp
grep -n 'kStackVma' kernel/proc/user_launch.cpp
```

构建:

```bash
cmake -B build -S . && cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
```

想看 mmap 真干活:内核测试里 `sys_mmap` 那几条(匿名映射、写、读回、`munmap` 释放)就是端到端验证。用户程序直接调 `mmap` 要等后面 musl 铺好 C 库的 `_syscall6` 封装——但内核侧的能力,这一章就到位了。
