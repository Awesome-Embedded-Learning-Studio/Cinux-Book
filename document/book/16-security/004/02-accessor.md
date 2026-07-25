---
title: 02 · 修法一:撤全局 stac,改局部 accessor
---

# 修法一:撤全局 stac,改局部 accessor

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
