---
title: 02 · 代码路线:从 MSR 装配到 `#GP` 来源判定
---

# 代码路线:从 MSR 装配到 `#GP` 来源判定

## usermode_init_asm:三只 MSR 怎么摆

[usermode.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/usermode.S) 的 `usermode_init_asm` 依次写 STAR、SFMASK、EFER。STAR 那段是最容易写错的一句:

```asm
    movq $0x08, %rdx            # 内核 CS 选择子
    shlq $16, %rdx              # 移到 EDX[31:16], 对应 STAR[63:48]
    orq  $0x08, %rdx            # 顺便在 EDX[15:0] 放 0x08, 对应 STAR[47:32]
    xorq %rax, %rax             # 低 32 位 EAX = 0 (STAR[31:0] 不用)
    movq $0xC0000081, %rcx
    wrmsr                       # EDX:EAX → MSR[RCX]
```

为什么是 `shlq $16` 而不是直觉上的 `shlq $32`?因为 `wrmsr` 只认 32 位的 `EDX` 和 `EAX`,把它们的值拼成 `EDX:EAX` 写进 64 位 MSR——它**不读** 64 位 `RDX` 的高 32 位。所以哪怕你 `shlq $32` 把 `0x08` 移到 `RDX` 的高半区,`wrmsr` 也照样只拿 `EDX`(低 32 位)那部分,结果 `STAR[63:48]=0`,`SYSRET` 推出的 CS 就变成 `0x13`(内核数据段 `0x10` | RPL3)——数据段选择子,CPU 在数据段上取指。正确做法是 `shlq $16`,让值落在 `EDX` 的 `[31:16]` 里——这正是 `wrmsr` 会写进 `STAR[63:48]` 的位置。注释里那行 `# %rdx<<16→%rdx: shift to EDX[31:16] for STAR[63:48]` 就是在反复提醒自己这件事。这段逻辑在 host 单元测试里用纯算术镜像过:`(0x08ULL << 16) | 0x08ULL == 0x00080008ULL`。

SFMASK 那段就直白得多,写 `0x200`(屏蔽 IF 位)。但正如上一节说的,SYSRET 不读它,这一章写了也不影响功能;加上 QEMU 的模拟还有坑(调试现场案二),所以测试里只验「写 `0x200` 不 `#GP`」,不断言读回值。

EFER 是读改写:先 `rdmsr` 读出当前值,`orq $1,%rax` 置 `SCE` 位(bit 0),再 `wrmsr` 写回。`SCE` 是 SYSRET/SYSCALL 指令的总开关,不开的话 `sysretq` 直接 `#UD`。

## jump_to_usermode:SYSRET 的寄存器契约

`jump_to_usermode` 按 System V AMD64 约定收参数:`%rdi=entry`、`%rsi=user_stack`、`%rdx=arg`。它把这几个值搬到 SYSRET 指定的位置:

```asm
    movq %rdi, %rcx            # entry → RCX (SYSRET 装进 RIP)
    movq %rsi, %rsp            # user_stack → RSP (SYSRET 不动 RSP, 软件切栈)
    movq %rdx, %rdi            # arg → RDI (用户入口的第一参数)
    pushq $0x202               # RFLAGS: IF(bit9) + 保留位1(bit1)
    popq  %r11                 # → R11 (SYSRET 从 R11 恢复 RFLAGS)
```

这里每一个 `movq` 都对应 SYSRET 硬件契约里的一条,错一个就崩。`movq %rsi,%rsp` 这行尤其要盯住:SDM §5.8.8 明说 SYSRET 不修改 RSP,所以**必须**在 `sysretq` 之前把栈指针切到用户栈,否则用户态会接着用内核栈,隔离和正确性全毁。`pushq $0x202 / popq %r11` 是个把立即数塞进 `R11` 的小技巧——不能直接 `mov` 立即数进 `R11`(虽然能,但用栈更直白),`0x202` 是 `IF` 位(0x200)加上 RFLAGS 里恒为 1 的保留位 1(0x002)。这样用户态一进去中断就是开的(否则第一条指令要是再触发什么异常都收不到)。

紧接着是一串 `xorq %rN,%rN` 把其余通用寄存器清零。这步不是为了 SYSRET,是为了**安全**:内核的寄存器里可能残留着内核地址、内核数据,把这些原样带进 Ring 3 等于把内核信息泄漏给用户态。虽然本 demo 的「用户程序」只是内核里硬编码的四字节、没有恶意,但这个习惯从第一天起就该养成——进 Ring 3 之前,把你不想让用户看到的寄存器全擦干净。

最后 `sysretq` 一跳,CPL 从 0 变 3,RIP=entry,隔离正式生效。

## GDT 用户段 + TSS.RSP0 + IST1

[gdt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/gdt.cpp) 的 `init()` 里,用户代码段、用户数据段这两个描述符其实早就填好了(010 章引入大内核 GDT 时就备好了),`GDT_USER_CODE=0x1B`、`GDT_USER_DATA=0x23` 这两个选择子常量也一直在。这一章新增的是 TSS 这块的实化:

```cpp
// 给 IST1 配一块独立的 4 KB Double Fault 栈
tss_.ist[0] = reinterpret_cast<uint64_t>(&df_stack_[sizeof(df_stack_)]);

const auto tss_addr = reinterpret_cast<uint64_t>(&tss_);
entries_[5] = tss_low_entry(tss_addr, sizeof(TaskStateSegment) - 1);
entries_[6] = tss_high_entry(tss_addr);
```

`df_stack_[]` 是 GDT 类里一块 `alignas(16) uint8_t[4096]` 的静态数组,`tss_.ist[0]` 指向它的**栈顶**(数组末尾,因为栈向下生长)。`TaskStateSegment` 是 104 字节,`static_assert(sizeof(TaskStateSegment)==104)` 把布局锁死;host 测试里镜像了一份 `TestTSS`,断言 `ist` 的偏移是 36、`iomap_base` 是 102、总大小 104——这些数字和 SDM §8.7 的 64 位 TSS 格式(Figure 8-11)对得上。

`tss_set_rsp0` 是个静态方法,写 `g_gdt.tss_.rsp[0]`:

```cpp
void GDT::tss_set_rsp0(uint64_t rsp0) {
    g_gdt.tss_.rsp[0] = rsp0;
}
```

`rsp[0]` 就是 RSP0。为什么这一章非要设它?因为 Ring 3 一旦触发异常(比如我们的 `cli` → `#GP`),CPU 要切回 Ring 0 执行异常处理,而**切到哪个内核栈**就是由 TSS.RSP0 决定的——CPU 从用户栈换到 RSP0 指向的内核栈,在上面压入 `SS/RSP/RFLAGS/CS/RIP/ERROR_CODE`,再跳进 IDT 里的处理程序。RSP0 要是悬空,异常一来栈就乱了。本 demo 用 `launch_first_user` 当时的内核 `rsp` 当 RSP0,够用——撞墙后内核直接 `halt`,不返回用户态,不需要复杂的栈管理。

## launch_first_user:用户地址空间的编排

[usermode.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/usermode.cpp) 的 `launch_first_user()` 是把上面所有零件串起来的编排函数。它分几步走,每一步都回答一个具体问题。

「用户程序」是什么?是硬编码在内核里的四字节字节流:

```cpp
constexpr uint8_t kUserCode[] = {
    0xFA,           // cli   —— 特权指令, Ring 3 执行必 #GP
    0xF4,           // hlt
    0xEB, 0xFC,     // jmp rel8 -4 (跳回 cli, 无限循环)
};
```

如实说:这不是从磁盘加载的 ELF 用户程序,没有 user libc,也没有 `user/linker.ld`——`user/` 目录是 023 才会出现的。这四字节存在的唯一目的,是让 `cli` 在 Ring 3 被执行一次,从而证明特权指令被拦住。

用户地址空间怎么搭?建一个独立的 `AddressSpace`,映一页代码在 `USER_ENTRY_BASE(0x400000)`,映 4 页栈在 `USER_STACK_TOP(0x7FFFFF000)` 下面,全部带 `kUserPageFlags = FLAG_PRESENT|FLAG_WRITABLE|FLAG_USER`:

```cpp
AddressSpace user_space;                              // 独立页表
uint64_t code_phys = g_pmm.alloc_page();
user_space.map(USER_ENTRY_BASE, code_phys, kUserPageFlags);
// ... 把 kUserCode 拷进 code_phys (走内核 higher-half 映射写) ...

uint64_t stack_base = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;
for (i = 0..USER_STACK_PAGES)
    user_space.map(stack_base + i*PAGE_SIZE, g_pmm.alloc_page(), kUserPageFlags);
```

`0x400000` 是 x86-64 上 ELF 默认的加载基址,`0x7FFFFF000` 落在用户半区高位、距离 32 GB 边界(`0x800000000`)只差一页,host 测试把它们都验过页对齐、都在用户半区(`< 0x800000000000`)、彼此不重叠。

接着是这一章最反直觉、也是踩坑最狠的一步——激活用户地址空间前,要把内核 PDPT 里的 **framebuffer identity-mapping** 条目复制进用户 PDPT。`AddressSpace` 构造时只复制了 PML4 的高半区(`[256..511]`,内核共享那部分),低半区全清零。可 framebuffer 用的是 identity mapping(物理地址直接当虚拟地址),落在低半区的 PDPT[3]。一旦 `activate()` 切了 CR3,那条 1 GB 大页就消失了——而 `kprintf` 写 console 正是要访问 framebuffer。后果是 `kprintf` 触发 `#PF`、需求分页又给那地址映了个普通 RAM 页、然后 fault handler 内部又 `kprintf` 又访问 framebuffer……连环 `#PF` 把串口输出搅成一串乱码。所以激活前得手动把缺失的 identity-mapping 条目搬过去:

```cpp
auto* kern_pdpt = ...;  auto* user_pdpt = ...;
for (uint32_t i = 0; i < PT_ENTRIES; i++)
    if ((kern_pdpt[i] & FLAG_PRESENT) && !(user_pdpt[i] & FLAG_PRESENT))
        user_pdpt[i] = kern_pdpt[i];   // 只补用户缺的, 不覆盖
```

之后才是 `user_space.activate()`(切 CR3)、设 `TSS.RSP0=当前 rsp`、调 `jump_to_usermode(USER_ENTRY_BASE, USER_STACK_TOP, 0)`。注意函数最后那句 `kprintf("[USER] ERROR: jump_to_usermode returned!\n")` 在本 demo 里**永远不会执行**——`jump_to_usermode` 一去不回,用户态撞墙后 `fatal_halt()` 永久停机。

## walk_level 的 user_flag:四级页表逐级传 user 位

这是 bug 三的修复,也是最值得讲清的一处。x86-64 的权限检查遍历**全部四级**页表:PML4 → PDPT → PD → PT。一条虚拟地址要让 Ring 3 能访问,不是「最终 PT 项有 user 位」就够——中间任何一级(PML4/PDPT/PD 条目)缺了 user 位,整条路径就判定为「权限不足」,触发 `#PF` 错误码 `0x05`(P=1 页存在、W/R=0 是读、U/S=1 是用户发起)。

原来的 `walk_level` 在分配新的中间页表(PDPT/PD)时只设了 `FLAG_PRESENT|FLAG_WRITABLE`,漏了 `FLAG_USER`。于是哪怕 `VMM::map` 最终给 PT 项带了 `FLAG_USER`,中间几级没有,用户态照样访问不了。修复是给 `walk_level` 加一个 `user_flag` 参数,从 `VMM::map` 提取 `FLAG_USER` 一路传下去:

```cpp
PageEntry* walk_level(PageEntry* table, uint64_t index, bool should_alloc, uint64_t user_flag = 0) {
    // ... 分配新表页时:
    entry.raw = new_page | FLAG_PRESENT | FLAG_WRITABLE | user_flag;
    //                                                ^^^^^^^^^ 逐级传下去
}
```

`VMM::map` 里:

```cpp
uint64_t user_flag = flags & FLAG_USER;     // 从调用者要的 flags 里抠出 user 位
auto* pdpt = walk_level(pml4_table, PML4_INDEX(virt), true, user_flag);
auto* pd   = walk_level(pdpt, PDPT_INDEX(virt), true, user_flag);
auto* pt   = walk_level(pd, PD_INDEX(virt), true, user_flag);
```

这样无论映用户代码页还是用户栈页,从 PML4 到 PT 四级全部带上 user 位,Ring 3 才进得去。这个坑的隐蔽之处在于:它和别的 bug(下面案一里的 framebuffer、STAR 移位)会互相掩盖——中间页表缺 user 位的 `#PF` 先发作,把 STAR 移位错误的症状藏了起来,你以为修好了其实还差一层。

## handle_gp:用 (cs & 0x03) 区分来源

[exception_handlers.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/exception_handlers.cpp) 的 `handle_gp` 这一处改动很小,意义却重:

```cpp
void handle_gp(InterruptFrame* frame) {
    dump_registers(frame, "#GP", 13);
    bool from_user = (frame->cs & 0x03) != 0;     // RPL 低 2 位 != 0 即来自 Ring 3
    if (from_user) {
        kprintf("[EXCEPTION] #GP at RIP=%p from user mode (Ring 3)\n", ...);
        kprintf("[EXCEPTION] Privileged instruction executed in Ring 3 -- protection works!\n");
    } else {
        kprintf("[FATAL] General Protection Fault in kernel mode (error code=%p)\n", ...);
    }
    fatal_halt();
}
```

为什么 `cs & 0x03` 能区分来源?段选择子的低 2 位是 RPL(Requester Privilege Level),异常压栈时 CPU 把当时的 CS(连同 RPL)存进 `InterruptFrame`。Ring 3 里执行指令时 `CS=0x1B`,`0x1B & 0x03 = 3`,非零;内核态 `CS=0x08`,`0x08 & 0x03 = 0`。所以一句位与就能告诉我们「这条 `#GP` 是用户撞墙,还是内核自己出了岔子」。前者正是隔离成立的信号,后者是内核 bug(本 demo 里不该发生)。host 测试把这两个分支都镜像过:`0x1B & 0x03` 判为 user、`0x08 & 0x03` 判为 kernel。

不管哪条分支,最后都 `fatal_halt()`——本 demo 里用户态撞墙就停机,不尝试恢复或杀进程,那是 023 之后的事。
