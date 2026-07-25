---
title: 061 · SMAP 遇上 SMP —— 撤掉全局 stac,给用户内存访问配 exception table
---

# 061 · SMAP 遇上 SMP —— 撤掉全局 stac,给用户内存访问配 exception table

> 上一章(056)开 SMAP 的办法,是在所有从用户态进来的入口各挂一对 `stac`/`clac`——`syscall_entry` 一进来就 `stac`,三个中断宏在「从用户态进」的分支里也 `stac`。等于一进内核就把 RFLAGS.AC 拉高,整段内核态都放行用户内存访问。单核下这套没毛病。可一旦上了 SMP,它就变成一颗定时炸弹:**RFLAGS.AC 是 per-CPU 位,而 context_switch 切任务时根本不存 RFLAGS**。任务从 CPU0 迁到 CPU1,新核上的 AC 是它自己的旧值(多半是 0),代码却还在按「全局 stac 已开」的旧假设裸解引用用户指针——撞上 AC=0 的核,SMAP #PF。`-smp 2` 下 shell 反复 `/hello`,`sys_waitpid` 写用户 `*status` 必崩。
>
> 这一章把那颗炸弹拆了,顺手还掉上一章末尾埋的债。两件事:其一,**撤掉入口级的全局 stac**,改成局部 `stac`/`clac` 的 user accessor——只在真正拷贝用户内存的那一小段窗口里放行 AC,拷完立刻关;所有「内核直接解引用用户指针」的路径迁到 accessor 上,syscall 按Linux 的样子切成 `do_*_kernel`(纯内核逻辑,可以 block)/ `sys_*`(薄边界,只管跨用户)两层。其二,**给 accessor 配上 exception table**——拷贝中途真 fault 了(用户传了个不可映射的地址),靠一张 RIP-based 的 `__ex_table` 把执行改到 fixup,accessor 返回 false,syscall 返回 `-EFAULT`,而不是 panic。这正是 056 章最后那句伏笔:「完整的 `copy_from_user`/`copy_to_user`(带 exception table 的容错访问)是后面的事」。
>
> C 档:验证不靠「用户可见的新能力」,靠两件可观测的事——accessor fault 的负测试(解引用未映射地址,返回 false 而不是把内核炸了)、exception table 纯函数的 host 单测,加上全量测试在单核和 `-smp 2` 下都不回归(各 962/0)。
>
> 一条诚实的边界先说在前头:**本机是 WSL2,不透传 SMAP**,所以「SMAP 真拦了一次内核访问用户页」在本机看不到。但这一章要验证的两件事都不依赖 SMAP 真生效——accessor fault 的负测试走的是内核态 accessor 指令的 fault(由 exception table 拦,跟 SMAP 开没开无关),分层和窗口纪律更是纯代码正确性。SMAP 真生效要换真机或 TCG;accessor 做对,跟环境无关。

## 这章咱们要点亮什么

1. **为什么「入口级全局 stac」天生跟 SMP 过不去**:两个事实叠一起就够——RFLAGS.AC 是 per-CPU 位,而 context_switch 的 `CpuContext` 里压根没有 RFLAGS 这一格。这不是某个边角 case,是模型本身跟 SMP 冲突。
2. **局部 stac/clac 的 accessor 取代全局 stac**:`access_ok` 先把坏范围挡掉,真正拷贝就是一扇 `stac` → 拷 → `clac` 的小窗,这扇窗**绝不阻塞、绝不调度**——否则又是 AC 跨核丢失的重演。
3. **syscall 切两层,铁律才有落点**:把「可以 block、可以摧毁地址空间、可以持内核状态」的内核逻辑(`do_*_kernel`)和「碰用户内存」(`sys_*`)彻底切开,「block 的时候不持用户指针」这条纪律才落得下来。
4. **exception table:accessor fault 的容错契约**:一张启动时排好序的 `__ex_table`,每条记录「这条 accessor 指令 fault 了就跳到这个 fixup」;PF handler 在最前面查它,命中就改 `frame->rip`,accessor 返回 false,syscall 返回 `-EFAULT`。这是 Linux `copy_from_user` 的契约,替代了之前「靠 demand-page 兜底、真不行就 panic」的赌法。

## 上一章埋的雷:全局 stac 在 SMP 下会丢

先把上一章怎么开的 SMAP 回顾一下,雷就埋在那套挂法里。

056 开 SMAP 时,`stac` 挂在两个地方。一是 SYSCALL 入口,因为 SYSCALL 必从用户态进来,handler 一定要读用户内存,所以一进来就放行:

```asm
swapgs
stac                # set AC: handler may touch user memory
```

(`syscall.S:66`。`stac` 原挂在该 `swapgs` 下一行,现已注释,见 :67。)二是中断入口,三个 ISR 宏各挂一对,而且只在「这个中断是从用户态进来的」分支里 `stac`(跟 `swapgs` 同条件),内核态中断继承当前 AC——这点上一章解释过,是为了不破坏嵌套里正在进行的 `copy_from_user`。

单核下这套为什么没毛病?因为只有一个 CPU。AC 在那个核上 `stac` 设上了,就一直在,直到某个出口 `clac`。任务切来切去都在同一个核,RFLAGS 是同一份。

**SMP 下这套假设就塌了。** 问题不在哪一行代码,在两个物理事实:

**事实一:RFLAGS.AC 是 per-CPU 位。** 每个核有自己的 RFLAGS,AC 是其中的 bit 17。CPU0 上 `stac` 设的是 CPU0 的 AC,CPU1 的 AC 它管不着。

**事实二:context_switch 切任务时不存 RFLAGS。** 看 `CpuContext` 这个结构——它就是 context_switch 存取任务现场的那块内存,布局写在 `context_switch.S` 头注释里:

```
Offset 0:  r15
Offset 8:  r14
Offset 16: r13
Offset 24: r12
Offset 32: rbp
Offset 40: rbx
Offset 48: rsp
Offset 56: rip
Offset 64: gs_base      (RESERVED — GS is per-CPU)
Offset 72: kgs_base     (RESERVED — KERNEL_GS_BASE is per-CPU)
Offset 80: fs_base      (per-thread TLS)
```

(`context_switch.S:9`。)这里头存了 callee-saved 通用寄存器、栈指针、返回地址、还有 per-thread 的 FS base。**没有 RFLAGS。** 函数头也直说了:`Clobbers: flags`(`context_switch.S:39`)——它压根就不保存标志寄存器。

两个事实一叠,炸弹就响了。想象任务 A 在 CPU0 上跑 `sys_waitpid`,走到了「写用户 `*status`」这一步——上一章的全局 stac 假设此刻 AC=1,代码直接 `*status = exit_code` 裸写用户内存。偏偏这时钟中断来了,A 被切走,调度器把它扔到 CPU1 上接着跑。CPU1 的 RFLAGS.AC 是多少?是 CPU1 自己的值——它从没替 A 执行过 `stac`,AC 大概率是 0。可代码还在按「全局 stac 已开」的旧假设继续裸写 `*status`。AC=0 + SMAP 开着 + 写用户页 = SMAP #PF。

实证很干脆:`-smp 2` 下 shell 反复 `/hello`,跑着跑着 `sys_waitpid` 这一写就撞上 AC=0 的核,页故障。查 pte 不是页表坏了(不是 0 就是合法映射),是 AC 丢了。

> 根因一句话:**「全局 stac」这个模型跟 SMP 在物理上不兼容**。它假设「一进内核 AC 就全程放行」,可 AC 是 per-CPU 的,根本没有「全局」可言。只要任务会跨核迁移,「入口处设的 AC」和「实际执行时的 AC」就可能不是同一份。修它不是打补丁,是把模型换掉。

## 修法一:撤全局 stac,改局部 stac/clac 的 accessor

正解对齐 Linux:别再「一进内核就放行」,改成**只在真正要碰用户内存的那一小段窗口里放行**,拷完立刻关。这套东西收拢在一个头文件里——`user_access.hpp`,提供 `access_ok` / `copy_to_user` / `copy_from_user` / `put_user` / `get_user` 这一组 accessor。

先看门槛 `access_ok`。它是个**纯范围检查**,不缺页、不走页表,只看地址落不落在用户半区:

```cpp
inline bool access_ok(const void* addr, size_t size) {
    uint64_t a = reinterpret_cast<uint64_t>(addr);
    if (a == 0) return false;                       // NULL
    uint64_t end;
    if (__builtin_add_overflow(a, size, &end)) return false;  // 回绕
    uint64_t last = (size == 0) ? a : end - 1;
    return cinux::arch::is_user_vaddr(a) && cinux::arch::is_user_vaddr(last);
}
```

(`user_access.hpp:66`。)它比上一章那个 `validate_user_ptr` 严:旧的只查 canonical 形式,**会放过内核高半地址**——用户传一个内核地址进来,旧检查说「canonical,过」,内核就照着读了,这是个潜在的安全绕过面。`access_ok` 把 NULL、内核高半、`addr+size` 回绕全挡掉。做基础设施时顺手把这类「旧实现偏宽」的点收紧,比留到后面踩雷强。

真正拷贝的是 `copy_to_user` / `copy_from_user`,内核态分支长这样(另一边 `copy_from_user` 对称):

```cpp
bool ok = true;
asm volatile(
    "stac\n"
    "1: rep movsb\n"
    "   clac\n"
    "   jmp 3f\n"
    "2: clac\n"                      // fixup:fault 时 AC 还是 1,关窗 + 置失败
    "   xorl %k[ok], %k[ok]\n"
    "3:\n" _ASM_EXTABLE(1b, 2b)
    : [ok] "+r"(ok), "+c"(n), "+D"(dst), "+S"(src)
    :
    : "memory");
return ok;
```

(`user_access.hpp:103`。)看这扇窗有多窄:`stac` 开门 → `rep movsb` 一条指令把整段拷完 → `clac` 关门。`rep movsb` 是**单 fault 点**(整段拷贝就这一条指令),所以拷贝中任何一字节 fault,故障 RIP 都精确指向 `1:`——这是后面 exception table 能精准拦截的前提。`_ASM_EXTABLE(1b, 2b)` 是给这条指令挂的注解,意思是「`1:` 这条 fault 了就去 `2:`」,先记着,下一节细讲。

> **这扇窗有一条铁律,比放行 AC 本身还重要:窗口绝不阻塞、绝不调度。** 理由就是上一节的根因——AC 是 per-CPU 的,context_switch 不存它。要是在 `stac` 之后、`clac` 之前你 `schedule_blocked` 了,任务被切走,再在 AC=0 的核上被切回来,等于带着放行态裸解引用用户内存——这正是咱们刚拆掉的雷。所以每个 accessor 就是 `stac` → 紧字节循环拷贝 → `clac`,中间不碰任何会阻塞的东西。

这条铁律有个推论,叫 **block-then-write**:凡是会阻塞的 syscall——`read` 等键盘、`waitpid` 等子进程——不能边阻塞边碰用户内存。正解是先在**内核缓冲区**上 block(此时 AC=0,安全),等重新 runnable 了,再用 accessor 把内核缓冲 `copy_to_user` 出去。阻塞发生在不持用户指针的时候,拷贝发生在一扇绝不再阻塞的小窗里。这条纪律贯穿后面所有的 syscall 改造。

accessor 就位,就能把上一章那个「入口级全局 stac」撤了。改动很小,就是把 `syscall_entry` 那条 `stac` 注释掉,三个 ISR 宏里从用户态分支的 `stac` 也注释掉:

```asm
# stac  (P3: global STAC removed -- SMAP real; user mem only via accessor stac)
```

(`syscall.S:67`、`interrupts.S:75`、`interrupts.S:191`、`interrupts.S:307`——三个 ISR 宏各挂一对,共三处。)出口的 `clac` 留着(无害,AC 本来就关着)。撤完之后,内核默认 AC=0,任何裸解引用用户指针都是 bug、都会被 SMAP 拦(真机/TCG 下)。合法的访问全部走 accessor 的 `stac` 小窗。这就是「SMAP 真生效」——不是「入口放行一大片」,是「除了 accessor 那扇小窗,哪都不放行」。

## 修法一的载体:syscall 切两层

accessor 准备好了,但还有个现实问题:上一章的 syscall handler 满地都是「直接解引用用户指针」。一个个改成 accessor,工作量是一方面,更麻烦的是——很多 handler **既要碰用户内存,又要 block**。比如 `sys_read`:它要从用户 buf 写回数据(碰用户内存),可 fd=0 读键盘时要 block 等输入。你要是在同一个函数里边 block 边 accessor,就违反了刚立的窗口铁律。

Linux 的解法是**把 handler 切两层**,照搬过来:

- **`do_*_kernel(...)`**:纯 kernel-to-kernel 的逻辑。收的是 kernel 指针 / kernel buf,**可以 block、可以 unmap 旧用户页、可以持任何内核状态**——因为它根本不碰用户内存。测试和内核内部直接调它。
- **`sys_*(...syscall 参数)`**:薄薄一层边界。先用 accessor 把用户参数(path、buf、argv…)搬进内核暂存(小的放栈、大的 `kmalloc` 放堆),再调 `do_*_kernel`。block 发生在 `do_*_kernel` 里(AC=0 安全),回来后 `sys_*` 再用 accessor 把结果搬回用户。

举个具体的——path 家族。原来 `resolve_user_path` 拿到用户 path 地址后,直接 `path[0] == '\0'` 裸读用户字节,然后整条字符串裸遍历。七个 path syscall(open / openat / creat / mkdir / chdir / unlink / rmdir)外加 stat 尾巴全经它,是最大一块裸解引用源头。改造后多了个 `read_user_path`:

```cpp
bool read_user_path(uint64_t path_virt, char* out, size_t cap) {
    if (!cinux::user::access_ok(reinterpret_cast<const void*>(path_virt), 1)) {
        return false;
    }
    size_t len = 0;
    while (len + 1 < cap) {
        char c;
        if (!cinux::user::get_user(&c, reinterpret_cast<const char*>(path_virt + len)))
            return false;
        if (c == '\0') break;
        out[len++] = c;
    }
    char term = 0;
    if (!cinux::user::get_user(&term, reinterpret_cast<const char*>(path_virt + len)))
        return false;
    if (term != '\0') return false;   // cap 内没 NUL,路径太长
    out[len] = '\0';
    return len > 0;
}
```

(`path_util.cpp:16`。)`get_user` 一次读一个字节,每读一字节就是一扇 AC 小窗(开、读、关)。`access_ok` 先把坏地址挡掉,循环逐字节读到 NUL,再确认 NUL 真的在 cap 范围内(防越界)。`resolve_user_path` 改成先 `read_user_path` 把用户 path 暂存到堆上的 `PathBuf`——为什么是堆不是栈?因为 4 KB 的 `char[PATH_MAX]` 加上 canonicaliser 的 scratch,会撑爆 16 KB 的内核栈,这是早先踩过的坑。

read / write / stat / signal / execve / 杂项,每个家族都照这套切:`do_read_kernel(fd, kbuf)` 在内核 buf 上 block,`sys_read` 用 accessor 搬进搬出;`do_stat_kernel(resolved_path, kst)` 做 VFS lookup 写 kernel stat,`sys_stat` 用 `copy_to_user` 搬给用户;`do_execve_kernel(kpath, kargv, kenvp)` 收 kernel 字符串、**可以 unmap 旧用户页**(execve 本来就要摧毁调用方地址空间,这一步必须在没持用户指针的时候做)。block-then-write 的样板就是 `sys_read`:block 在 `do_read_kernel` 里读键盘到内核 buf,runnable 后 `sys_read` 再 `copy_to_user`。

> 切两层不是洁癖,是正确性的载体。没有这层切开,「block 时不持用户指针」这条铁律根本没地方落——你没法在一个既碰用户内存又会 block 的函数里保证窗口不跨 schedule。分层之后,block 永远在 `do_*_kernel`(AC=0),碰用户内存永远在 `sys_*` 的 accessor 小窗(不 block),两者物理隔离。

## 修法二:给 accessor 配 exception table

accessor 的 `access_ok` 能挡掉「一看就坏」的地址,可挡不掉「地址合法、范围合法,但那一页偏偏没映射」的情况——用户传了个落在他地址空间里、却从没被 demand-page 映射的地址。`rep movsb` 拷到那一页,#PF。

这一章之前,这种 fault 是这么兜的:PF handler 看 fault 地址,试着 demand-page 给它映射个零页(对那些「该有页但还没分配」的合法情况),实在不可映射就 panic。这有两个毛病。其一,**demand-page 会把坏指针也默默映射成零页**,accessor 读到一串 0 还返回 true——用户的 bug 被掩盖了。其二,**真不可映射的地址直接 panic**,内核挂掉。更要命的是第三点:因为 accessor 只能返回 bool(成功/失败),而失败又只来自 `access_ok` 预检,**「accessor 解引用非法用户指针应当返回 -EFAULT」这条根本没法测**——你一传坏地址,内核就 panic 了,测试写不下去。

Linux 的正解是 exception table,照搬。思路一句话:**给每条可能 fault 的 accessor 指令挂个注解,记下「这条 fault 了就跳到这个 fixup」;PF handler 一看故障 RIP 命中注解,就把返回地址改成 fixup,accessor 从 fixup 接着跑,返回失败。**

第一步,给 linker 一个专门收这些注解的 section。`linker.ld` 里:

```
__ex_table : AT(ADDR(__ex_table) - KERNEL_VMA) ALIGN(8) {
    __start___ex_table = .;
    KEEP(*(__ex_table))
    __stop___ex_table = .;
}
```

(`linker.ld:76`。)放在 `.init_array` 后头,`ALIGN(8)` 因为每条记录是 16 字节(两个 quad),`KEEP` 防 gc-sections 把它当没用的扔了。两个符号 `__start___ex_table` / `__stop___ex_table` 圈出表的边界。section 名故意不带前导点(叫 `__ex_table` 不是 `.__ex_table`),对齐 Linux,避开 ld 通配符 `*(.__ex_table)` 的点号歧义。

表里每条记录就俩字段:

```cpp
struct ExceptionTableEntry {
    uint64_t fault_rip;    // fault 的 accessor 指令地址(那个 rep movsb)
    uint64_t fixup_rip;    // 该跳去哪(clac + 置失败 那段)
};
```

(`extable.hpp:32`。)挂注解的宏,回头看那个 accessor 内联汇编里的 `_ASM_EXTABLE(1b, 2b)`:

```cpp
#define _ASM_EXTABLE(fault_lbl, fixup_lbl)                              \
    ".pushsection __ex_table,\"a\"\n"                                   \
    ".balign 8\n"                                                       \
    ".quad " #fault_lbl "\n"                                            \
    ".quad " #fixup_lbl "\n"                                            \
    ".popsection\n"
```

(`extable.hpp:102`。)它干的事是:在汇编到 `1:`(那个 `rep movsb`)的时候,顺手切到 `__ex_table` section,写下「`1:` 的地址、`2:` 的地址」这一对 quad,再切回来。所以每个 accessor 被实例化一次,表里就多一条记录,说「这个实例的 `rep movsb` fault 了,去它自己的 `2:`」。

查表得快,所以**启动时排一次序**(`sort_extable`,在 IDT 起来、中断还没开的时候,空表也是 no-op),之后就能二分:

```cpp
inline const ExceptionTableEntry* extable_search(const ExceptionTableEntry* begin,
                                                 const ExceptionTableEntry* end, uint64_t rip) {
    while (begin < end) {
        const ExceptionTableEntry* mid = begin + (end - begin) / 2;
        if (mid->fault_rip == rip) return mid;
        if (mid->fault_rip < rip) begin = mid + 1;
        else end = mid;
    }
    return nullptr;
}
```

(`extable.hpp:41`。)排序用插入排序,不用 qsort——freestanding 内核没 libc,而且表也就几十条,插入排序够。这俩函数都是纯函数(接 `[begin, end)` 迭代器),所以能直接拿 host 单测罩住,不用塞进内核跑。

最后是接线——PF handler 最前面查它:

```cpp
void handle_pf(InterruptFrame* frame) {
    uint64_t fault_addr;
    __asm__ volatile("movq %%cr2, %0" : "=r"(fault_addr));

    if ((frame->cs & 0x03) == 0) {                       // 内核态门
        if (const auto* entry = cinux::arch::search_exception_tables(frame->rip)) {
            frame->rip = entry->fixup_rip;               // iretq 回 fixup(clac + ok=false)
            return;
        }
    }
    // ... 后面才是 demand-page / CoW / 栈守卫 / panic
}
```

(`page_fault.cpp:74`。)`handle_pf` 已从 `exception_handlers.cpp` 拆出,独立成 `page_fault.cpp`,这是该 tag 当时结构的实情。读 CR2 之后、demand-page 之前,先判两件事:**是不是内核态 fault**(`cs & 3 == 0`)、**故障 RIP 命不命中表**。都满足,把 `frame->rip` 改成 fixup,直接 return。`iretq` 一弹,执行就从 fixup 接着跑——fixup 干的是 `clac`(fault 时 AC 还是 1,得关窗,不然 AC 泄漏成 SMAP 旁路)+ 清 ok,accessor 返回 false,syscall 拿到 false 返回 `-EFAULT`。

> 上面这段代码为讲解已简化。真实实现还多一层 `should_demand_page` 先查:若 fault 落在合法用户 VMA 且 `error_code` 指示是 not-present(`!P`),先 demand-page 那一页再 resume `rep movsb`,而不是直接 fixup 返回 `-EFAULT`——否则大 buffer(如 `read`/`write`)跨过一页还没摸到的 malloc/mmap 页,会被误判成坏指针返回 `-EFAULT`。只有「真不在合法 VMA」(或 P 位已置的硬 fault)才走 fixup。这是 Linux uaccess 的同款做法。真实实现见 `page_fault.cpp:74` 起的 `F-EXTABLE` 注释段。

> 这里有两个边界要划清,都是故意的设计。
>
> **其一,只拦内核态 accessor RIP。** 用户态 fault(`cs & 3 != 0`)直接跳过这张表,走原来的 demand-page 不变。用户的栈页、heap 页第一次访问时缺页,是 F2 lazy-allocation 的正常范式,该 demand-page 就 demand-page,exception table 不掺和。它是专门给「内核代用户访问,用户却传了坏地址」这种内核态 fault 准备的精准拦截。
>
> **其二,demand-page / CoW / 栈守卫 / NULL-deref 这些正常内核 fault,它们的 RIP 不是 accessor 指令,查表 miss,原逻辑一个字不改。** exception table 只对被 `_ASM_EXTABLE` 注解过的那几条 `rep movsb` 生效。所以它接进来是「纯增益」:没注解的 fault 行为完全不变,注解了的 fault 从 panic 变成 `-EFAULT`。

## 验证:三个角度,绕开「SMAP 在本机不生效」

本机不透传 SMAP,所以验证不能靠「触发一次 SMAP 拦截看效果」——那个本机永远看不到。换成三个不依赖 SMAP 真生效的角度。

**角度一:exception table 的纯函数,host 单测罩住。** `extable_search`(二分)和 `extable_sort`(插入排序)都是纯函数,host 直接测:空表、单元素、命中头中尾、miss、gap 在上下,七例全过(`test_extable`,7 passed / 0 failed)。这验证了查表逻辑本身是对的,跟内核环境无关。

**角度二:accessor fault 的负测试,实证 exception table 真拦得住。** 给 `copy_from_user` / `copy_to_user` 传一个**真未映射**的用户地址,看它是不是返回 false 而不是把内核炸了:

```cpp
// test_user_ptr.cpp:test_extable 命名空间
void test_copy_from_unmapped_returns_false() {
    char kbuf[16];
    // 0x7000000000:落在用户半区,但既不在 identity/direct-map 覆盖区,
    // 也不在 mmap/brk/栈范围 —— 真未映射,rep movsb 必 fault
    bool ok = cinux::user::copy_from_user(kbuf, (const void*)0x7000000000ULL, sizeof(kbuf));
    TEST_CHECK(!ok);   // 期望 false:extable 拦住 fault,没 panic
}
```

(`test_user_ptr.cpp:153`。)`copy_from_user` 一执行,`rep movsb` 第一字节就 #PF,handle_pf 查表命中,改 `frame->rip` 到 fixup,accessor 返回 false。测试看到 false,PASS。要是没有 exception table,这一下就 panic 了,测试根本跑不到断言。这个测试走的是**内核态 accessor 指令的 fault**,由 exception table 拦,跟 SMAP 开没开无关——所以本机能验。

> 这个负测试有个坑值得记一笔:地址不能乱挑。头一版用 `0x40000000`(1 GB 用户址),结果返回 true——因为测试内核的 identity/direct-map 把物理 RAM 映射到了 1 GB 往上,这个地址碰巧有映射,`rep movsb` 成功读完不 fault,exception table 没机会拦。教训:accessor fault 的负测试地址,得确认它**没被任何内核映射覆盖**(identity map / direct-map / mmap / brk / 栈),用高位用户地址(481 GB 那种)避开。

**角度三:全量测试两 leg 不回归。** accessor 化和分层动了几乎每一个 syscall,最大的回归风险就在这。`run-kernel-test-all` 跑两 leg——单核和 `-smp 2`,各 962 passed / 0 failed。单核 leg 跳过 AP 唤醒(没 AP),`-smp 2` leg 真启动 AP1、读回 cr4=0x300620(SMEP/SMAP 位都在)、efer=0xd01(NXE),AP 机制回读 PASS。两 leg 全绿,说明这一通「撤全局 stac + accessor + 分层 + extable」的大改造没把既有路径弄坏。

> 有个老朋友在这章里又露了一面,值得点一下。`run-kernel-test` 的测试都是直接调 `do_*_kernel`、传内核地址——**根本不碰 accessor**,所以 SMAP 在测试内核下压根不触发。这就是上一章说的「假绿」同一个根:测试用内核地址,accessor 的真闸碰不到。accessor 的真闸是 ring-3 的 musl smoke(真用户地址)。可本机 SMAP 不透传,smoke 也就验不了「SMAP 真拦」。所以这一章能本机闭环验证的,就是上面三个角度;accessor 在真用户路径下的正确性,代码照已验证模式写、靠真机/TCG 兜底。诚实交代,不假装在本机验全了。

## 这一个 tag 还顺带带进来了什么

最后交代一句,免得你 checkout 这个 tag 看到一堆「这章没讲」的东西犯迷糊。这一章对应的源码增量是个大块,除了上面两条主线,整弧 diff 还夹带了几样别的东西,它们的教学分别留到后面的章节:

- **TTY 行规范 + 阻塞读 + 键盘接通**(一批):用户态终端的行规范(termios UAPI、Ctrl+C 生成信号字符、stdin 阻塞读替忙等)。这是 F10 用户态运行时的下一块,单独成一章(062)细讲。
- **CoW 写时复制的 U 位门控松绑**:内核态写 CoW 用户页的 panic 门松了一点。
- **`-smp 2` 下 fork exit/reap 的跨核修复**:子进程在另一个核上退出、父进程 reap 的时序竞态,AP idle 重建。
- **几条 CI 红线**(测试调度器代码量、forktest 的 C99 写法)。

这几样跟「SMAP-SMP + exception table」是同一个源码增量里一起进来的(patch-replay 按 CinuxOS 的时间序整弧落地,保 1:1),但它们各自有各自的教学脊梁,挤进这一章只会喧宾夺主。这一章只管把 SMAP 和 exception table 这两条线讲透;剩下的,该哪章哪章。

## 小结

- 上一章开 SMAP 用的「入口级全局 stac」,在 SMP 下跟物理冲突:RFLAGS.AC 是 per-CPU 位,而 `CpuContext` 不存 RFLAGS——任务跨核迁移就丢 AC,裸解引用用户内存撞 SMAP #PF。
- 修法是换模型:撤全局 stac,改局部 `stac`/`clac` 的 accessor;accessor 窗口绝不阻塞(block-then-write);syscall 切 `do_*_kernel`(可 block、不碰用户)/ `sys_*`(accessor 边界)两层,让铁律有落点。
- accessor 的 fault 容错交给 exception table:`__ex_table` 收注解,启动排序,PF handler 二分查表,命中改 `frame->rip` 到 fixup,accessor 返回 false、syscall 返回 `-EFAULT`,不再 panic。这是上一章末尾那笔债的偿还。
- 验证绕开「本机 SMAP 不生效」:extable 纯函数 host 单测 + accessor fault 负测试(内核态 RIP,不依赖 SMAP)+ 全量两 leg 不回归。
