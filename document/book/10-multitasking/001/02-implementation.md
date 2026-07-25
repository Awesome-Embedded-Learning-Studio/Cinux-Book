---
title: 02 · 代码路线:PID/fork/CoW/execve/waitpid/syscall
---

# 代码路线:PID 分配器 / fork / CoW / execve / waitpid / syscall

## PID 分配器:为什么不用一个自增计数器

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
    mutable Spinlock lock_;              // 注:034 时无此字段;后来为
                                         // SMP-safe 加锁(F-QA Q4d / DEBT-005)
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

## fork:复制一切,除了该另起的那些

`fork()` 在 [fork.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/fork.cpp),逻辑很直白——**先把父进程整个 TCB 抄过去,再把「不该继承的」逐个改掉**:

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

## CoW 页表:共享 + 写保护 + 写时复制(以及那个没有的引用计数)

fork 如果老老实实把父进程的每一页用户内存都抄一份,代价巨大——子进程可能一辈子都不写其中 99% 的页。Cinux 的做法是 **Copy-On-Write**:fork 时只复制**页表结构**,真正的数据页**共享**,等谁真的要写了,再为它单独复制一页。

实现是递归走 4 级页表。外层循环处理 PML4 的用户半区(下标 0..255),给每个在用的 PML4 项分配一个新页表页,然后 `copy_page_table_level` 递归往下:

```cpp
void copy_page_table_level(uint64_t src_phys, uint64_t dst_phys, int level) {
    // 注:034 时为三参签名;后来扩为五参,加了 virt_base 与
    // cinux::mm::IVMAStore& vmas(见 fork.cpp / process_internal.hpp)。
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

第一,**`handle_cow_fault` 没接进 `#PF` handler**。034 的 [page_fault.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/page_fault.cpp) 里 `handle_pf` 只做 demand-paging——错误码的 present 位为 0(页不存在)时补一页,其余(包括 CoW 的写保护故障,present=1)一律 `dump_registers` + 打一行 `[FATAL] Page Fault` + `fatal_halt`。也就是说,真去写一张被 fork 标成只读 + COW 的页,在 034 会**直接停机**,而不是走 `handle_cow_fault`。这个函数写好了、却没有任何调用方——典型的「为下一步备好、本步未启用」的死代码。把它真正接进 `#PF`、让它端到端跑起来,是接下来的活。

第二,**CoW 没有引用计数**。`copy_page_table_level` 只是把双方改成共享 + 只读 + COW,并不记「这张物理页现在被几方共享」;`handle_cow_fault` 每次都无条件分配新页 + 复制,也不更新「另一方」的 PTE。就算把上一条接上,这套也只够「一次 fork、父子各写各的」用——多方共享(fork 之 fork)和原始页回收都不保证。

所以 034 的 CoW 是个**诚实的半成品**:页表标记和 fault handler 的逻辑都铺好了,host 单测也覆盖了「父/子两方、标记转换、写后隔离」这些 PTE 级语义,但从「写一张 CoW 页」到「自动复制成私有页」的端到端通路,这一章还没合上。

## execve:换掉整个进程映像,只留下 PID

`execve` 是「换」。它读一个 ELF 程序,把当前进程的用户空间**整个换掉**,但 PID、父进程、调度关系**原封不动**。核心是 [execve.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/execve.cpp) 里那条流水线,前面设计图已经画了,这里看最精华的两段:清旧映像、铺新段。

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

注意 execve 在 034 **只**设了入口地址,**没有**搭用户栈、也**没有**把 `argv`/`envp` 铺进去(参数在 `sys_execve` 里被 `(void)` 掉了)。真正跳进新程序的用户态(`jump_to_usermode`)是调用方的活,这一章把映像铺好、入口备好就交差。把 argv/envp 和用户栈补上是后续的事——这也是为什么 [execve.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/execve.hpp) 的注释会说「caller is responsible for jumping to the new entry point」。

> ELF 校验本身在 [elf_types.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/elf_types.cpp) 的 `validate_elf_header` 里:魔数 `0x7F 'E' 'L' 'F'`、类别 64 位、小端、机型 x86-64、类型 `ET_EXEC`、program header 偏移/尺寸合法、至少一个 program header。任一不过就返回对应的 `ElfValidateResult`,再映射成 `ExecveResult::BadElf*`(统一归到 `-ENOEXEC`,即 -8)。`Elf64_Ehdr` 恰好 64 字节、`Elf64_Phdr` 恰好 56 字节,都用 `static_assert` 钉死——packed 结构体的尺寸绝不能错,host 单测也专门验这两个数。

## waitpid:收尸,别留 zombie

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

## 五个系统调用怎么接到分发表

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
