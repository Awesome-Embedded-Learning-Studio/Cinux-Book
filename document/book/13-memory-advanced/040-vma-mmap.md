---
title: 040 · VMA 与 mmap
---

# 040 · VMA 记账与 mmap:给地址空间一本账,让用户程序能动态映射

> 从这章起进 **F2 内存弧**。F2 是 CinuxOS ROADMAP 里点名的**最大结构瓶颈**——它阻塞 mmap、brk、Page Cache、CoW、共享内存这一整串东西。040 立这条弧的地基:**VMA(Virtual Memory Area)记账**——给 `AddressSpace` 配一本"区域账本",再在它上面实现 `mmap`/`munmap`/`mprotect` 三个 syscall。之后用户程序才真正能动态要内存、映射文件。
>
> A 档:这次用户态摸得到——写个程序 `mmap` 一块匿名页、读写它,是真做得到的(后面 F10 musl 铺好前,测试里直接 C++ 调 `sys_mmap` 验)。

## 这章咱们要点亮什么

两件事,一根线(VMA 是账本,mmap 是账本上第一个真用户):

1. **VMA 记账**:`AddressSpace` 多了一个 `LinkedListVMAStore`——按起始地址有序的区域账本,insert 会合并相邻同权限、remove 会拆分中间。
2. **mmap / munmap / mprotect**:三个 Linux syscall 落地。`mmap` 懒分配(只记 VMA,真要物理页时 PF 触发 demand paging);`munmap` 拆分释放;`mprotect` 改权限。

## 为什么需要 VMA

040 之前,`AddressSpace` 只有**页表**——页表回答"这个虚拟页映射到哪个物理页、什么权限",但回答不了**"这段虚拟地址是什么"**:它是匿名的还是文件映射的?是栈还是堆?可读可写可执行吗?属于哪个 inode?

这个问题在只有 execve 一次性铺好用户布局时还能忍——段是固定的。可一旦用户程序自己要动态内存(`mmap`/`brk`)、要共享内存、要按需读文件(Page Cache),没有一个"区域"层来记账,每个机制都得自己想办法记下"自己管着哪段地址",口径不一、互相打架。

VMA 就是这个**单一事实源**:一段虚拟地址 `[start, end)` + 权限 + 属性(匿名/文件/栈/堆)+ backing(文件映射时记 inode)。mmap/brk/demand paging/CoW/共享内存都来问它、都来改它。

## VMA 数据结构

`kernel/mm/vma.hpp`:

```cpp
// vma.hpp:51 —— 区域属性(位标志)
enum class VmaFlags : uint64_t { Read, Write, Exec, Anonymous, Shared, Stack, Heap, ... };
constexpr VmaFlags operator|(VmaFlags a, VmaFlags b) noexcept;  // :62
constexpr VmaFlags operator&(VmaFlags a, VmaFlags b) noexcept;  // :66 —— mprotect 要 & 提取 base

// vma.hpp:105 —— 抽象接口(将来可换红黑树)
class IVMAStore { ... virtual ErrorOr<void> insert(...) = 0; virtual void remove(...) = 0; ... };

// vma.hpp:142 —— 链表实现
class LinkedListVMAStore : public IVMAStore { ... };
```

`LinkedListVMAStore` 是侵入式双向链表,按 `start` 有序。两个关键操作:

- **insert 合并**:插入一段新区域时,如果和相邻区域权限相同、地址连续,就**合并成一个**大区域(不让账本碎片化)。
- **remove 拆分**:在一段中间取消映射(比如 `munmap` 掉一大段里的一小块),要把原来的 VMA **拆成两半**。

store 自己 owns 节点(RAII),不用调用方管生命周期。`IVMAStore` 留了抽象接口——现在用链表(实现简单、区域少时够快),将来区域多了想换红黑树,只换实现、不动调用方。

## AddressSpace 集成

`kernel/mm/address_space.hpp`:

```cpp
// :124 —— 访问账本
IVMAStore& vmas() { return vma_store_; }
// :150 —— 直接对象成员(非指针,RAII)
LinkedListVMAStore vma_store_;
// :152 —— 串行化(PF 路径、mmap 都会改账本)
Spinlock vma_lock_;
```

账本是 `AddressSpace` 的**直接成员**(不是指针),随 `AddressSpace` 一起构造/析构。配一把 `vma_lock_`,因为 page fault 路径和 syscall 路径都会查/改它。

**注册点**(哪些地方往账本里记):

- **execve** 加载 ELF 时,每个 `PT_LOAD` 段(按 `PF_W`/`PF_X` 转 `VmaFlags`)记一条;
- **用户栈**:init 里建好栈页后,记一条带 `Stack` flag 的 VMA(`kernel/proc/init.cpp:120`):

```cpp
// init.cpp:120-123
constexpr cinux::mm::VmaFlags kStackVma =
    cinux::mm::VmaFlags::Read | cinux::mm::VmaFlags::Write | cinux::mm::VmaFlags::Stack;
task->addr_space->vmas().insert(stack_base, cinux::arch::USER_STACK_TOP, kStackVma);
```

栈 VMA 带 `Stack` flag 是给后面的 demand paging 用的——栈向下增长,PF 命中 `Stack` VMA 下方时,自动扩栈。

## 一个关键决策:PF 查询先做"诊断",不做"硬门控"

这里有个**故意**的妥协,值得讲清楚。有了 VMA 账本,本来该做一件顺理成章的事:page fault 时查 VMA,**没命中的就直接 segfault**(这才是 Linux 的行为——访问没登记的区域 = 野指针 = 杀进程)。

但 040 **没这么做**,只做了**诊断增强**:PF 时查一下 VMA,没命中就 `klog_warn` 一声,但**仍然照常 demand page**(给页)。为什么?因为现有的 demand paging 对任何 not-present 地址都宽松映射(野指针、栈下方都给页),想改成"无 VMA → 真 segfault"得**重构整个 demand paging**(VMA 硬门控 + 栈自动下扩 + guard 页),漏判一处就是 shell halt,高风险。所以 M1 选了零风险的诊断路径,真 segfault 留到 M5 demand paging 重构。

> 教训:重构要会**分阶段**。"有账本了就该立刻严格门控"是直觉,但严格门控依赖 demand paging 本身先改好。先把账本立起来当诊断用,等 demand paging 重构时再拧紧——比一次性大爆炸稳。

## mmap / munmap / mprotect

账本有了,三个 Linux syscall(`kernel/syscall/sys_mmap.hpp`)就建在它上面:

```cpp
// sys_mmap.hpp:47 / :59 / :71 —— 6 参,匹配 SyscallFn
int64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t offset);
int64_t sys_munmap(uint64_t addr, uint64_t length, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_mprotect(uint64_t addr, uint64_t length, uint64_t prot, uint64_t, uint64_t, uint64_t);
```

- **mmap 懒分配**:只 `find_free_area`(或 `MAP_FIXED`)+ `insert` 一条 VMA,**不映射物理页**。首访时 PF 走现有 demand paging 给页。匿名映射记 `Anonymous`,文件映射记 backing inode(`file->inode`,内容留 M4 Page Cache demand-read)。
- **munmap**:遍历范围内的 demand-paged 页,`translate`→`unmap`→`free_page`(用户页不是 direct-map,unmap 安全——和 GOTCHA #7 不冲突),再 VMA `remove`(拆分)。未映射区域返 0(POSIX 语义)。
- **mprotect**:`find` 覆盖的 VMA,保留它的 base 属性(用 `operator&` 提取 `Anonymous`/`Stack`/...),换掉 R/W/X,然后 `remove`+`insert`(拆分)+ 遍历已映射页 re-issue PTE 权限。

> 注意三个 syscall 都声明成 **6 参**,哪怕 `munmap` 只用前两个——因为它们要注册到 syscall 表里,类型必须匹配统一的 `SyscallFn`(6 参)。`sys_munmap` 后面几个参数是哑的。

还有一条:**fork 现在会复制 VMA**。fork 的 CoW 页表克隆之后,遍历父的 VMA `insert` 到子(backing 指针共享,文件内容由 M4 demand-read)。这样子进程继承父的内存布局——这是 fork/exec 能正常工作的前提。

## 踩坑(从迁移笔记搬来)

- **`klog_warn` 是宏,不是命名空间函数**:不能写 `cinux::lib::klog_warn(...)`,展开会变 `cinux::lib::::cinux::...` 语法错。用裸 `klog_warn(...)`。这一坑 M1 踩过、M2 又差点踩。
- **`operator new` 失败返 nullptr,不是 panic**:crt_stub 的 `operator new` 直接 `return g_heap.alloc()`,失败返 nullptr(内核禁异常,没法抛)。VMA 节点小 + heap 会自动 expand,OOM 实际不发生,沿用惯例。但写代码时要心里有数。
- **backing 类型一开始写错了**:M1 把 VMA 的 backing 写成 `InodeOps*`(grep `inode.hpp` 只看见 `class InodeOps`),实际 `file->inode` 是 `Inode*`(`InodeOps` 只是 vtable)。M2 才改对。**grep 看类型时,要把主类型和它的 ops 区分开。**

## 验证

```bash
# VMA 账本 + AddressSpace 集成
grep -rn 'class LinkedListVMAStore\|class IVMAStore\|enum class VmaFlags' kernel/mm/vma.hpp
grep -n 'vma_store_\|vmas()' kernel/mm/address_space.hpp
# 三 syscall
grep -rn 'int64_t sys_mmap\|int64_t sys_munmap\|int64_t sys_mprotect' kernel/syscall/
# init.cpp 栈 VMA 注册
grep -n 'VmaFlags::Stack\|vmas().insert' kernel/proc/init.cpp
```

构建 + 内核测试(这一弧 run-kernel-test 从 039 的 705 涨到 721:VMA store +7,mmap/munmap/mprotect +9):

```bash
cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
cmake --build build --target run-kernel-test
```

想看 mmap 真干活,内核测试里 `sys_mmap` 那几条(匿名映射 round-trip、`MAP_FIXED`、`munmap` 释放)就是端到端验证;用户程序直接调要等 F10 musl 铺好 `_syscall6` wrapper。

## 小结与下一站

F2 的地基立起来了:`AddressSpace` 有了 VMA 账本,`mmap`/`munmap`/`mprotect` 三个 syscall 让用户程序能动态要内存。PF 门控暂时还是诊断(留 M5),但账本已经在那儿,后面拧紧就是。

下一站 **041** 接着 F2 往上走:`brk`(给 `malloc` 用的堆扩展)+ **Page Cache**(文件内容缓存,文件映射 demand-read 的真正数据源)。
