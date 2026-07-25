---
title: 04 · 修法二:给 accessor 配 exception table
---

# 修法二:给 accessor 配 exception table

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
> **其一,只拦内核态 accessor RIP。** 用户态 fault(`cs & 3 != 0`)直接跳过这张表,走原来的 demand-page 不变。用户的栈页、heap 页第一次访问时缺页,是 lazy-allocation 的正常范式,该 demand-page 就 demand-page,exception table 不掺和。它是专门给「内核代用户访问,用户却传了坏地址」这种内核态 fault 准备的精准拦截。
>
> **其二,demand-page / CoW / 栈守卫 / NULL-deref 这些正常内核 fault,它们的 RIP 不是 accessor 指令,查表 miss,原逻辑一个字不改。** exception table 只对被 `_ASM_EXTABLE` 注解过的那几条 `rep movsb` 生效。所以它接进来是「纯增益」:没注解的 fault 行为完全不变,注解了的 fault 从 panic 变成 `-EFAULT`。
