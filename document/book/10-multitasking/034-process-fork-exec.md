---
title: 034 · fork / execve / waitpid:让进程能生、能换、能收尸
---

# 034 · fork / execve / waitpid:让进程能生、能换、能收尸

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

## 代码路线

### PID 分配器:为什么不用一个自增计数器

进程要身份证号,最省事的写法是一个全局自增计数器。但那样 PID 会**无限增长**,而且号永远不复用——跑久了 PID 就溢出。Cinux 的选择是一个**有界池 + 回收**:[pid.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/pid.hpp) 定义的 `PidAllocator`:

```cpp
class PidAllocator {
public:
    static constexpr int PID_NONE = 0;   // 0 保留(空闲/失败哨兵)
    static constexpr int PID_MAX  = 256;
    int  alloc();                        // 分一个空闲 PID,池满返回 PID_NONE
    void free(int pid);                  // 回收,可安全重复 free / 越界 free
    bool is_allocated(int pid) const;
    int  count() const;
private:
    bool in_use_[PID_MAX + 1];           // 下标 0..256,0 不用
    int  next_hint_;                     // 下次从这儿开始找
};
```

`alloc()` 不是每次都从头扫,而是从 `next_hint_` 开始、**绕一圈**找第一个空闲位,找到就把 hint 推到后面:

```cpp
int PidAllocator::alloc() {
    for (int i = 0; i < PID_MAX; ++i) {
        int candidate = next_hint_ + i;
        if (candidate > PID_MAX) candidate -= PID_MAX;   // 绕回
        if (candidate == 0) candidate = 1;               // 跳过保留的 0
        if (!in_use_[candidate]) {
            in_use_[candidate] = true;
            next_hint_ = (candidate >= PID_MAX) ? 1 : candidate + 1;
            return candidate;
        }
    }
    return PID_NONE;   // 256 个全占满
}
```

两个细节值得一说。第一,**为什么是「绕一圈」而不是「从头扫」**:用 hint 是为了避免每次分配都从 PID 1 开始扫一遍(进程多了会变成 O(n) 的累赘),从上一次分配的位置接着找,通常一步到位。第二,`free()` 反过来会把 hint **往回拉**到刚释放的那个更低 PID:

```cpp
void PidAllocator::free(int pid) {
    if (pid <= 0 || pid > PID_MAX) return;   // 越界/哨兵:直接无视
    if (!in_use_[pid]) return;               // 没在用:重复 free,安全 no-op
    in_use_[pid] = false;
    if (pid < next_hint_) next_hint_ = pid;  // 把更低的空闲号顶到前面,下次优先复用
}
```

这样「分配顺序」和「复用最低空闲号」两全:刚释放的低号会很快被下一次 `alloc` 拿到(host 单测里 `free(2)` 之后下一次 `alloc()` 果然返回 2)。256 这个上限对教学内核够用,真要像 Linux 那样撑到几百万进程,得换位图 + 更聪明的分配策略,但那是以后的事。

### fork:复制一切,除了该另起的那些

`fork()` 在 [process.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/process.cpp),逻辑很直白——**先把父进程整个 TCB 抄过去,再把「不该继承的」逐个改掉**:

```cpp
int fork(PidAllocator& pid_alloc) {
    auto* parent = Scheduler::current();
    ...
    int child_pid = pid_alloc.alloc();          // ① 新 PID

    auto* child = new (std::align_val_t{alignof(Task)}) Task;
    std::memcpy(child, parent, sizeof(Task));   // ③ 整个 TCB 原样拷贝

    // ④ 修掉子进程专属字段
    child->tid    = next_tid.fetch_add(1, ...);
    child->pid    = child_pid;
    child->ppid   = parent->pid;                // 父子关系就这一行
    child->state  = TaskState::Ready;
    child->parent = parent;
    child->children = nullptr;
    child->exit_status = 0;
    ...
}
```

为什么要先 `memcpy` 再改,而不是从头构造?因为 TCB 里绝大多数东西子进程都该和父进程**一样**:优先级、调度类、当前工作目录 `cwd`、FPU 状态……一个个手抄容易漏。整块拷过去,再定点改那几个身份字段(`tid`/`pid`/`ppid`/`state`/`parent`/`children`/`exit_status`),既不容易错,也更贴合 fork 的语义——「除了身份,其余照搬」。

但有两样东西**绝不能共享**,必须给子进程另起:一个是**内核栈**,一个是**地址空间**。先看内核栈:

```cpp
// ⑤ 给子进程分配全新的内核栈(16KB),并拷贝父栈的「已用区」
uint64_t child_stack_virt = alloc_stack_vaddr(TaskBuilder::STACK_PAGES);
uint64_t stack_size = TaskBuilder::STACK_PAGES * cinux::arch::PAGE_SIZE;
...  // 把 4 页物理内存 map 到 child_stack_virt,底部写 STACK_MAGIC

uint64_t parent_stack_used = parent->kernel_stack_top - parent->ctx.rsp;
std::memcpy(
    reinterpret_cast<void*>(child_stack_virt + stack_size - parent_stack_used),
    reinterpret_cast<void*>(parent->ctx.rsp),
    parent_stack_used);

// ⑥ 调整子的 RSP,让「已用区」在新栈里位于同样的相对位置
child->kernel_stack     = child_stack_virt;
child->kernel_stack_top = child_stack_virt + stack_size;
child->ctx.rsp = child_stack_virt + stack_size - parent_stack_used;
```

这段是 fork 里最容易看晕的部分,值得拆开讲。`parent->ctx.rsp` 是父进程**上一次被调度器切进来时**保存下来的栈指针;`kernel_stack_top - ctx.rsp` 就是当时栈「已经用了多少」。子进程要复制的是这一段「已用区」(它决定了子进程恢复时栈上有合法的返回链),而且要把它摆在**新栈的同样高度**——所以子的 `ctx.rsp = 新栈顶 - 同样的 used`,让拷过去的字节和 `ctx.rsp` 对齐。底部照例写一个 `STACK_MAGIC`(0xDEADC0DE),栈溢出时这块魔法字被踩烂,就能被发现。

> 内核栈必须新开,不能共享,道理很硬:父子是两个独立调度实体,各自有自己的调用栈,共用一个栈会在切换时互相踩烂。这一条,和「地址空间可以 CoW 共享」是两码事——别混。

地址空间的共享才是真正的巧思,也就是下一节的 Copy-On-Write。

### CoW 页表:共享 + 写保护 + 写时复制(以及那个没有的引用计数)

fork 如果老老实实把父进程的每一页用户内存都抄一份,代价巨大——子进程可能一辈子都不写其中 99% 的页。Cinux 的做法是 **Copy-On-Write**:fork 时只复制**页表结构**,真正的数据页**共享**,等谁真的要写了,再为它单独复制一页。

实现是递归走 4 级页表。外层循环处理 PML4 的用户半区(下标 0..255),给每个在用的 PML4 项分配一个新页表页,然后 `copy_page_table_level` 递归往下:

```cpp
void copy_page_table_level(uint64_t src_phys, uint64_t dst_phys, int level) {
    auto* src_table = phys_to_virt(src_phys);
    auto* dst_table = phys_to_virt(dst_phys);
    for (uint32_t i = 0; i < PT_ENTRIES; i++) {
        if (!src_table[i].is_present()) continue;
        if (level > 1) {
            // 中间层(PDPT/PD):分配新页表页,递归下一层
            uint64_t new_page = cinux::mm::g_pmm.alloc_page();
            ...  // 清零
            dst_table[i].raw = new_page | FLAG_PRESENT | FLAG_WRITABLE | FLAG_USER;
            copy_page_table_level(src_table[i].phys_addr(), new_page, level - 1);
        } else {
            // 叶子层(PT):共享物理页,可写页 → 双方改 RO + COW
            dst_table[i].raw = src_table[i].raw;            // 先指向同一物理页
            if (entry_flags & FLAG_WRITABLE) {
                dst_table[i].raw &= ~FLAG_WRITABLE;         // 子:去掉写
                dst_table[i].raw |= FLAG_COW;               // 子:置 COW
                src_table[i].raw  &= ~FLAG_WRITABLE;        // 父:也去掉写
                src_table[i].raw  |= FLAG_COW;              // 父:也置 COW
            }
        }
    }
}
```

最关键的是叶子层那四行:**父子两边的 PTE 都被改成只读 + `FLAG_COW`**。为什么父也要改?因为如果不改父的,父还是可写的,它一写就直接改了共享页,子的数据就被污染了——CoW 的前提是「写会触发 #PF」,所以**任何共享方都得是只读的**,谁的写都先撞 page fault,再走复制流程。

`FLAG_COW` 复用的是 PTE 的 **bit 9**。x86-64 的页表项里 bit 9-11 是「软件可用/硬件忽略」位(见 OSDev Page Tables 对 PTE 位域的说明),CPU 不会解释它们,内核拿来打自己的标记再合适不过。这也是本章 `paging_config.hpp` 唯一的新增:

```cpp
constexpr uint64_t FLAG_COW = 1ULL << 9;   // Available bit 9: Copy-On-Write marker
```

写时复制的活儿,设计上交给 `handle_cow_fault`:它先确认这是一次「CoW 故障」(在用 + 只读 + 带 COW 位),然后复制。代码长这样——但要先打个预防针:**这个函数在 034 写好了、却还没接进 `#PF` handler**(详见本节末尾),现在先看它的逻辑:

```cpp
bool handle_cow_fault(uint64_t fault_vaddr) {
    PageEntry* pte = get_pte(pml4_phys, fault_vaddr);
    if (!pte || !pte->is_present()) return false;
    if (pte->raw & FLAG_WRITABLE) return false;   // 可写页的 #PF 不是 CoW
    if (!(pte->raw & FLAG_COW))    return false;  // 没标记也不是 CoW

    uint64_t old_phys = pte->phys_addr();
    uint64_t new_phys = cinux::mm::g_pmm.alloc_page();
    // 把旧页 4096 字节拷到新页
    auto* src = reinterpret_cast<uint8_t*>(old_phys + KERNEL_VMA);
    auto* dst = reinterpret_cast<uint8_t*>(new_phys + KERNEL_VMA);
    for (uint64_t i = 0; i < cinux::arch::PAGE_SIZE; i++) dst[i] = src[i];

    pte->set_phys_addr(new_phys);   // 指向私有新页
    pte->raw |= FLAG_WRITABLE;      // 恢复写
    pte->raw &= ~FLAG_COW;          // 清掉 COW
    cinux::arch::flush_tlb(fault_vaddr & ~(cinux::arch::PAGE_SIZE - 1));  // 刷 TLB
    return true;
}
```

这里有两个**必须看清楚的边界**,都说明 034 的 CoW 是「搭好骨架、还没通电」。

第一,**`handle_cow_fault` 没接进 `#PF` handler**。034 的 [exception_handlers.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/exception_handlers.cpp) 里 `handle_pf` 只做 demand-paging——错误码的 present 位为 0(页不存在)时补一页,其余(包括 CoW 的写保护故障,present=1)一律 `dump_registers` + 打一行 `[FATAL] Page Fault` + `fatal_halt`。也就是说,真去写一张被 fork 标成只读 + COW 的页,在 034 会**直接停机**,而不是走 `handle_cow_fault`。这个函数写好了、却没有任何调用方——典型的「为下一步备好、本步未启用」的死代码。把它真正接进 `#PF`、让它端到端跑起来,是接下来的活。

第二,**CoW 没有引用计数**。`copy_page_table_level` 只是把双方改成共享 + 只读 + COW,并不记「这张物理页现在被几方共享」;`handle_cow_fault` 每次都无条件分配新页 + 复制,也不更新「另一方」的 PTE。就算把上一条接上,这套也只够「一次 fork、父子各写各的」用——多方共享(fork 之 fork)和原始页回收都不保证。

所以 034 的 CoW 是个**诚实的半成品**:页表标记和 fault handler 的逻辑都铺好了,host 单测也覆盖了「父/子两方、标记转换、写后隔离」这些 PTE 级语义,但从「写一张 CoW 页」到「自动复制成私有页」的端到端通路,这一章还没合上。

### execve:换掉整个进程映像,只留下 PID

`execve` 是「换」。它读一个 ELF 程序,把当前进程的用户空间**整个换掉**,但 PID、父进程、调度关系**原封不动**。核心是 [process.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/process.cpp) 里那条流水线,前面设计图已经画了,这里看最精华的两段:清旧映像、铺新段。

清旧映像靠 `clear_user_mappings`,**手工**把用户半区(PML4[0..255])的四级页表走一遍,把数据页和页表页逐层释放:

```cpp
void clear_user_mappings(cinux::mm::AddressSpace& space) {
    auto* pml4 = reinterpret_cast<PageEntry*>(space.pml4_phys() + KERNEL_VMA);
    for (uint32_t i = 0; i < 256; i++) {            // 只动用户半区
        ...  // PDPT → PD → PT 三层嵌套
             // 最里层:free 数据页,pt[l].raw = 0
             // 退回来:free PT 页,pd[k].raw = 0
             // 再退:  free PD 页;再退:free PDPT 页
    }
}
```

> 这函数有个**注释和代码打架**的地方:注释写着「Does NOT free the page table pages themselves」(不释放页表页),可代码明明把 PT/PD/PDPT 页都 `free_page` 了。这是典型的「注释过期、以代码为准」。提醒一句:**源码注释是线索,不是权威**——和 010 那回把 TSS 图号抄错是同一类教训,看到注释里的断言,拿代码核实一遍再信。

铺新映像是逐段、逐页的。对每个 `PT_LOAD` 段,按 `p_memsz` 算出它覆盖的页范围,逐页「分配 + 清零 + 拷文件字节 + 映射」:

```cpp
for (uint16_t i = 0; i < phnum; i++) {
    const auto& phdr = phdrs[i];
    if (phdr.p_type != elf::PT_LOAD) continue;          // 只管可加载段

    uint64_t seg_start = phdr.p_vaddr & ~(PAGE_SIZE - 1);
    uint64_t seg_end   = (phdr.p_vaddr + phdr.p_memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    uint64_t page_flags = FLAG_PRESENT | FLAG_USER;
    if (phdr.p_flags & elf::PF_W)  page_flags |= FLAG_WRITABLE;
    if (!(phdr.p_flags & elf::PF_X)) page_flags |= FLAG_NX;

    for (uint64_t vaddr = seg_start; vaddr < seg_end; vaddr += PAGE_SIZE) {
        uint64_t phys = cinux::mm::g_pmm.alloc_page();
        // ① 整页清零
        auto* dst = reinterpret_cast<uint8_t*>(phys + KERNEL_VMA);
        for (uint64_t b = 0; b < PAGE_SIZE; b++) dst[b] = 0;
        // ② 按 p_filesz 从 inode 拷文件字节(落在 filesz 之外的就是 BSS,保持零)
        uint64_t page_base_offset = vaddr - phdr.p_vaddr;
        if (page_base_offset < phdr.p_filesz) {
            uint64_t copy_len = phdr.p_filesz - page_base_offset;
            if (copy_len > PAGE_SIZE) copy_len = PAGE_SIZE;
            inode->ops->read(inode, phdr.p_offset + page_base_offset,
                             dst + page_base_offset, copy_len);
        }
        task->addr_space->map(vaddr, phys, page_flags);
    }
}
```

这段里藏着一个优雅的处理:**BSS(未初始化全局变量)是「免费」清零的**。ELF 的一个段有两个大小:`p_filesz`(文件里实际占多少)和 `p_memsz`(加载到内存要占多少)。数据段的 `p_memsz` 常常大于 `p_filesz`,多出来的那截就是 BSS。这里的做法是**先把整页清零,再只拷 `p_filesz` 范围内的文件字节**——于是 `filesz` 之外、`memsz` 之内那一段天然全是零,BSS 就这么落地了,不用单独处理。host 单测特意写了 `phdr.p_memsz > phdr.p_filesz` 的断言,守的就是这个语义。

页权限也照 ELF 段权限来:`PF_W` → 可写,没有 `PF_X` → 置 `NX`(不可执行)。这样代码段是 R+X、数据段是 R+W+NX,权限不混乱。

最后,入口只写一行:

```cpp
task->ctx.rip = ehdr->e_entry;
```

注意 execve 在 034 **只**设了入口地址,**没有**搭用户栈、也**没有**把 `argv`/`envp` 铺进去(参数在 `sys_execve` 里被 `(void)` 掉了)。真正跳进新程序的用户态(`jump_to_usermode`)是调用方的活,这一章把映像铺好、入口备好就交差。把 argv/envp 和用户栈补上是后续的事——这也是为什么 [process.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/process.hpp) 的注释会说「caller is responsible for jumping to the new entry point」。

> ELF 校验本身在 [elf_types.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/elf_types.cpp) 的 `validate_elf_header` 里:魔数 `0x7F 'E' 'L' 'F'`、类别 64 位、小端、机型 x86-64、类型 `ET_EXEC`、program header 偏移/尺寸合法、至少一个 program header。任一不过就返回对应的 `ElfValidateResult`,再映射成 `ExecveResult::BadElf*`(统一归到 `-ENOEXEC`,即 -8)。`Elf64_Ehdr` 恰好 64 字节、`Elf64_Phdr` 恰好 56 字节,都用 `static_assert` 钉死——packed 结构体的尺寸绝不能错,host 单测也专门验这两个数。

### waitpid:收尸,别留 zombie

子进程退出后,它的 TCB 不能立刻销毁——父进程可能还要来问「它退成啥样了」。所以退出进程先进 `Zombie` 状态(进程已死,但 TCB 还留着等收),父进程 `waitpid` 来「收尸」时才真正清理。`waitpid` 在 children 单链表里找目标:

```cpp
WaitpidResult waitpid(int pid, int* status, PidAllocator& pid_alloc) {
    auto* parent = Scheduler::current();
    ...
    if (pid != -1 && pid <= 0) return WaitpidResult::InvalidPid;   // 非法 PID
    if (parent->children == nullptr) return WaitpidResult::NoChildren;  // 没孩子 = ECHILD

    // pid == -1:找第一个 zombie;pid > 0:找指定 PID
    ...  // 扫链表定位 target,顺带记 prev 用于摘链
         // 找不到指定 PID → NotFound(ESRCH);孩子还没退 → NotExited

    if (status != nullptr) *status = target->exit_status;   // ① 收退出码

    // ② 从单链表摘掉
    if (prev != nullptr) prev->wait_next = target->wait_next;
    else                 parent->children = target->wait_next;

    pid_alloc.free(target->pid);                 // ③ 回收 PID
    target->state  = TaskState::Dead;            // ④ 标记彻底死亡
    target->parent = nullptr;
    return WaitpidResult::Ok;
}
```

这里复用了 `Task::wait_next` 这个本来给互斥锁/信号量等待队列用的侵入式链表指针,**兼作** children 链表的 next。一个字段两用,省得再加一个。`fork` 里挂 children 也是用它:`child->wait_next = parent->children; parent->children = child;`(头插法)。

有一个**和 Linux 不一样、必须讲清楚**的地方:034 的 `waitpid` 是**非阻塞**的。看 `sys_waitpid` 的包装:

```cpp
if (result == WaitpidResult::Ok)        return pid;        // 收到了,返回子 PID
if (result == WaitpidResult::NotExited) return 0;          // 孩子还没退 → 立即返回 0
return static_cast<int64_t>(result);                        // 其它错误 → 负 errno
```

Linux 的 `waitpid` 默认会**阻塞**等孩子退出;Cinux 034 这版孩子没退就直接返回 0(「现在没有可收的」),父进程要等就得自己轮询或靠别的方式。这是个有意识的简化——真正的阻塞等待要把父进程挂到等待队列上、等孩子 exit 时唤醒,那是更后面的工作。这一章先把「收尸」这条**同步**路径打通。

> 五个系统调用的错误码都**沿用 Linux errno 的数值**(`ENOENT=2`、`ECHILD=10`、`EISDIR=21`、`EINVAL=22`、`ENOEXEC=8`、`ENOMEM=12`、`ESRCH=3`),`ExecveResult`/`WaitpidResult` 的枚举值直接就是负的 errno,`sys_*` 包装原样返回。这样用户态拿到的返回值语义和 Linux 对得上,host/内核单测也对 errno 数值做了硬断言。

### 五个系统调用怎么接到分发表

最后一步是把这些能力暴露成系统调用。[syscall_nums.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/syscall_nums.hpp) 给五个新号:

```cpp
enum class SyscallNr : uint64_t {
    ...
    SYS_getpid  = 39,    // Linux x86_64: getpid
    SYS_getppid = 110,   // Linux x86_64: getppid
    SYS_fork    = 57,    // Linux x86_64: fork
    SYS_execve  = 59,    // Linux x86_64: execve
    SYS_waitpid = 61,    // Linux x86_64: waitpid
};
```

每个系统调用一个薄薄的 `sys_*` 封装(`sys_fork` 就是 `return fork(g_pid_alloc);`,`sys_getpid` 就是 `return current()->pid;`),然后在 [syscall.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/syscall.cpp) 的 `register_builtin_handlers()` 里统一注册进分发表:

```cpp
syscall_register(SyscallNr::SYS_getpid,  sys_getpid);
syscall_register(SyscallNr::SYS_getppid, sys_getppid);
syscall_register(SyscallNr::SYS_fork,    sys_fork);
syscall_register(SyscallNr::SYS_execve,  sys_execve);
syscall_register(SyscallNr::SYS_waitpid, sys_waitpid);
```

用户态 `syscall` 指令进来 → `syscall_dispatch` 按号查表 → 调对应 `sys_*`。和 023 那套 syscall 框架是同一套机制,只是表里多了五项。号码跟 Linux 对齐的好处再次体现:以后写用户态 libc 包装、甚至直接拿 Linux 的小程序过来改,syscall 号这一层不用动。

## 调试现场

按惯例这一节该讲真实踩坑。但 034 这个 tag **没有留下调试笔记**(`document/notes/034/` 是空的,context pack 的 notes 也是空)。按写作契约,没素材就不硬造——我不会编一段「某天 QEMU 里炸了 #GP,然后我发现……」的虚构故事来凑数。

但这不代表这一节没东西可讲。恰恰相反,把源码读细之后,034 有一个**真实存在、源码可证、却和教科书直觉相悖**的设计缺口,值得摆到台面上。这一节就讲它。

### 没有 notes,但有真相:034 的 fork 还没让子进程「返回 0」

`fork(2)` 的 man page 写得明明白白:**fork 在父进程里返回子进程的 PID,在子进程里返回 0**。这是 fork 最标志性的语义——同一个调用点,两条返回路径,靠返回值区分「我是谁」。Cinux 的 `process.hpp` 里 `fork` 的文档注释也这么写着:

> *Return value semantics (set in the child's TCB via ctx.rax): Parent: returns child PID; Child: returns 0*

听起来天经地义。但去代码里找「在子进程的 ctx.rax 里写 0」这件事——**找不到**。原因很硬:`CpuContext` 这个上下文结构体**根本没有 rax 字段**:

```cpp
struct alignas(16) CpuContext {
    uint64_t r15, r14, r13, r12, rbp, rbx;   // 只存 callee-saved
    uint64_t rsp, rip;
};
```

它只保存 **callee-saved 寄存器**(r15-r12、rbp、rbx)加 rsp、rip——这是 019 那会儿为协作式调度设计的:切换发生在已知的调用边界上,caller-saved 寄存器(包括 rax)本来就按约定算「已废」,不用存。所以上下文里没有 rax,`fork()` 里也没有任何一处去写子进程的 rax。

那子进程到底怎么「跑起来」的?回头看前面 fork 的第 ⑤⑥ 步:子进程的 `ctx` 是从父进程**整块 memcpy** 来的,内核栈也拷贝了「已用区」,`ctx.rsp` 对齐到新栈同样高度。这意味着子进程被调度器选中、`context_switch` 切进去时,它**从父进程上次被切进来时保存的 rip 处继续**,沿着调度器的返回路径往上走——就像它「刚被调度器唤醒」一样,而**不是**像 fork 系统调用刚刚 return 回来。

换句话说:034 的 fork **确实造出了子进程**(TCB、内核栈、CoW 页表、挂进 children、排进调度器,一应俱全),父进程也**确实**拿到了 `child_pid`(那是 `fork()` 同步返回给父进程的)。但「子进程从 fork 返回 0」这条**控制流**并没有被接上——子进程不会执行 `SYSRET` 带着 rax=0 回到用户态的 fork 调用点,它只是作为一个上下文副本被调度起来。

这不是「偶发性 bug」,而是**这一章的 fork 在语义上还没闭环**。证据不止源码一处,测试也在「让」着它:

- host 单测的 `fork_semantics` 全是**字面量模拟**——`int child_pid = 42; ASSERT_GT(child_pid, 0);`、`int child_return = 0; ASSERT_EQ(child_return, 0);`,根本没真跑 fork。
- 内核测试 `test_dispatch_sys_fork` 的注释直接挑明:测试环境不跑调度循环,`sys_fork`「会优雅失败、返回 -1」,断言放宽到 `ret == -1 || ret >= 0`。

没有一条测试在真刀真枪地验「fork 之后子进程看见 0、父进程看见 PID」——因为这个端到端语义在 034 还立不住。

为什么会留这个缺口?因为要把它补上,得动比较深的东西:要么给 `CpuContext` 加 rax(那就得改 019 以来的整套上下文/切换约定),要么给子进程造一个**手工的内核栈帧**,让它的 `ctx.rip` 指向一个「把 rax 设成 0 再走系统调用返回路径」的小 trampoline。两条路都不是「顺路」能干的活,而 034 这一章的重心是「把五大原语和 CoW 铺起来」,子进程返回值的闭环被自然地推到了后面。

这正是「调试现场」该传递的东西:不是「我踩了个坑然后修了」那种已经愈合的伤,而是**还没填、却在源码和测试里肉眼可见**的坑。而且它不是孤例——034 的进程三件套普遍处于「搭好骨架、尚未通电」的状态:fork 的子进程返回值没闭环、CoW 的 `handle_cow_fault` 没接进 `#PF`(真写 CoW 页会 fatal_halt)、`waitpid` 还是非阻塞。这些都不是偶发 bug,而是这一章有意识地把「把原语和机制铺好」做完了、把「让它们端到端跑通」留给了后面。顺带也提个醒:别盲信头文件里的文档注释,`process.hpp` 那句「set in the child's TCB via ctx.rax」描述的是一个**还没实现**的意图,结构体里压根没有 rax。注释是愿望,代码才是事实。

## 验证

和前面几章一样,034 分三层验证:纯逻辑 host 单测、QEMU 机内测试、端到端跑内核。

**第一层:host 单元测试。** PID 分配器、TCB 新字段、`FLAG_COW` 位运算、CoW PTE 状态机、syscall 号、`ExecveResult` 的 errno 映射、ELF 结构尺寸与校验——这些不碰真硬件,在 host 上 `-DCINUX_HOST_TEST` 编、链上 `pid.cpp` + `elf_types.cpp` 跑:

```bash
ctest --test-dir build -R fork_exec --output-on-failure
```

注意它**不**跑真 fork(那需要调度循环和真页表),只验「给我这个 PTE / 这个 PID 状态,算出来的结果对不对」——比如 `FLAG_COW` 标记转换、PID 分配的复用/耗尽、ELF 头校验的各种坏case。一次跑全部 host 测试:

```bash
cmake --build build --target test_host
```

**第二层:QEMU kernel 测试。** 真跑内核代码、走真 syscall 分发的机内测,在 `main_test.cpp` 里 scheduler 和 `syscall_init()` 之后注册了 `run_fork_exec_tests()`:

```bash
cmake --build build --target run-big-kernel-test
```

它验:getpid/getppid 直调和经 `syscall_dispatch` 两条路返回值一致(测试里临时装一个 `Task{pid=42,ppid=1}` 让 syscall 有 current 可读)、PID 分配器机内 smoke、`FLAG_COW` 位定义、CoW PTE 的「标记为只读+COW → 写后解析成私有可写页」转换、sys_fork 分发路径可达、ExecveResult 的 errno 数值、ELF64 头校验(合法头过、坏魔数/坏类别/坏机型/无 program header 各返回对应错误)。

**第三层:端到端。** 这一层 034 比较特殊——因为前面「调试现场」讲的那个缺口,「fork 出子进程、它 execve、父进程 waitpid 收到退出码」这条**完整**链路在 034 还没法漂亮地演示(子进程的返回值闭环没接上)。所以这一层更像是「看日志确认子系统起来了」:

```bash
cmake --build build --target run
```

留意串口里 fork/execve/waitpid 各自的 `[PROC] fork: created child pid=...`、`[EXECVE] loaded ... entry=...`、`[WAITPID] reaped child pid=...` 这几行。能在日志里看到它们被走到,说明五个 syscall 已经接进分发、底层设施各司其职。完整的「spawn 一个独立程序并等它结束」演示,要等下一章把 fork 的控制流闭环补上,才真正漂亮。

## 下一站

到 034,我们有了 fork、execve、waitpid 三个原语和 CoW 页表——进程能生、能换、能收。但有两件事还没收口:一是 fork 让子进程「返回 0」的控制流闭环,二是 waitpid 还是非阻塞的。这些是 034 留下的、源码里看得见的待办。

有了这些原语,一件自然而然的事变得可能:**给每个终端 spawn 它自己独立的 shell 进程**。033 的桌面只能有一个终端里转一个 shell;有了 fork+execve,我们可以 fork 出子进程、让它 execve 成 shell,父子用管道通信——于是桌面能同时开**多个**终端,每个跑自己的 shell,互不干扰。怎么把这些原语拼成「多终端桌面」,并顺手把 fork 那些还没填的坑填上,是下一章的事。

## 参考

- Linux man-pages — `fork(2)`:fork 返回语义(父进程得子 PID、子进程得 0、子进程是父进程的拷贝)。https://man7.org/linux/man-pages/man2/fork.2.html
- Linux man-pages — `execve(2)`:用新程序替换当前进程映像(新栈/堆/数据段)、`argv`/`envp` 语义。https://man7.org/linux/man-pages/man2/execve.2.html
- Linux man-pages — `waitpid(2)`:收尸 zombie 子进程、`ECHILD`、`status` 收集。https://man7.org/linux/man-pages/man2/waitpid.2.html
- OSDev Wiki — ELF:程序头、`PT_LOAD`、`p_filesz`/`p_memsz` 与 BSS 的关系。https://wiki.osdev.org/ELF
- OSDev Wiki — Page Tables:x86-64 页表项位域概览。其中 PTE 的 bits 9-11 是 CPU 不解释的「可用/忽略」位(这是 Intel SDM 与 AMD64 APM 定义的标准架构事实),`FLAG_COW` 正是复用其中的 bit 9。https://wiki.osdev.org/Page_Tables
