---
title: 056 · NX / SMEP / SMAP:用三个 CPU 位把内核和用户态隔开
---

# 056 · NX / SMEP / SMAP:用三个 CPU 位把内核和用户态隔开

> 之前内核和用户态之间的硬件隔离,基本只靠分页(用户页映射进用户地址空间、内核页不在用户空间露面)。但分页管的是「地址」,管不了「这一页能不能被执行」「内核能不能碰用户那一页」。这一章把 x86_64 的三个硬件保护位真正拨开:NX(用户数据页不可执行,落实 W^X)、SMEP(内核不可执行用户页)、SMAP(内核不可读写用户页)。三个位各管一格,合起来把「内核碰用户页」这个方向、连同「用户数据页不可执行」一起堵死。A 档:验证靠一个机制回读测试——读 EFER 看 NXE 设上、读 CR4 看 SMEP/SMAP 跟着 CPUID 走;再靠跑真程序(shell、GUI)开完这三 位不炸,证明内核路径没踩雷。
>
> 一条诚实的边界先说在前头:**本机是 WSL2 嵌套 KVM,它不透传 CPUID.07H:EBX**,所以 SMEP/SMAP 在开发机上**验证不了生效**——代码的 CPUID-gated 逻辑会正确地跳过它们。NX 不受影响(EFER 位是 x86_64 baseline,WSL2 透传),真生效。真机、完整 KVM、或 TCG 会暴露 SMEP/SMAP,这三个位就都活。这一章讲的是代码怎么做对,不是「在本机看到 SMEP 拦住了一次攻击」。

## 这章咱们要点亮什么

1. **三个位各管什么**:NX 管「用户数据页能不能被执行」,SMEP 管「内核能不能执行用户页」,SMAP 管「内核能不能读写用户页」——执行 + 访问,用户 + 内核,正好把隔离面铺满。
2. **NX 为什么不用 CPUID gate,而后两个必须 gate**:NX 是 x86_64 baseline,SMEP/SMAP 是 2011+ 的可选特性,写不支持的 CR4 位会直接 #GP。
3. **SMAP 的 stac/clac 之舞**:内核要碰用户内存(读参数、写返回值)时,得显式 `stac` 临时放行、用完 `clac` 关上;这条纪律得挂在所有从用户态进来的入口。
4. **怎么验证一个「开了但不一定生效」的机制**:靠机制回读(EFER/CR4 位)+ CPUID 跟随,而不是靠「触发一次攻击看它拦没拦」。

## 先把隔离面铺清楚:三个位各管一格

把内核态(Ring 0)和用户态(Ring 3)之间的硬件隔离拆开看,有两个维度:**能不能执行**、**能不能访问(读写)**。再乘上「谁对谁」(用户对用户数据、内核对用户页),就铺出一张表。NX、SMEP、SMAP 各占一格:

| | 执行 | 访问(读/写)|
|---|---|---|
| **用户态碰用户数据页** | **NX** 管(数据页不可执行) | (本就允许,无保护)|
| **内核态碰用户页** | **SMEP** 管(内核不可执行用户页) | **SMAP** 管(内核不可读写用户页)|

NX 是「用户能不能把自己的数据页当代码跑」(W^X 的那一半:写了的数据不能执行)。SMEP/SMAP 是反方向的:防止内核——通常是因为 bug 或被攻击者诱导——去执行/访问用户空间的页。经典提权攻击的路子就是诱骗内核跳到用户提前布置好的代码页去执行;SMEP 把这条路堵死(内核一执行用户页就 #PF),SMAP 连「读」也堵死(内核一碰用户页就 #PF,得显式放行才行)。三个位互补,缺哪个哪条路就漏。

下面挨个看怎么开。

## NX:拨下 EFER.NXE,让 bit-63 真的被当成 NX 位

NX(No-Execute)是这三个里最省心的,因为它是 x86_64 的 baseline:长模式用 PAE 页表,页表项的 bit-63 就是 NX 位。但这个位**只有在 EFER.NXE 设上之后才被解释成 NX**;NXE 没设时,bit-63 是 reserved,理论上设了会触发 reserved-bit #PF。所以开 NX 的总开关就一件事——设 EFER.NXE。

这一步在 `usermode.S` 里,和设 EFER.SCE(开 SYSCALL)挤在一条 `wrmsr` 里:

```asm
movq $0xC0000080, %rcx             # MSR_EFER
rdmsr                              # 读当前 EFER
orq $(1 | (1 << 11)), %rax         # SCE(bit0)|NXE(bit11):两个一起设
wrmsr                              # 写回
```

（`usermode.S:64`。)NXE 是 bit 11。为什么不用 CPUID 检测、无条件设?因为 NX 是 x86_64 baseline——任何能跑 64 位长模式的 CPU/QEMU 都支持,不存在「写了不支持」的情况(和下面 SMEP/SMAP 的待遇不同)。

设上 NXE 之后,页表路径里那些给页标 `FLAG_NX` 的地方才真正生效——NXE 没开时 bit-63 是 reserved 位,标了等于没标(还理论上危险)。这几处现在都活了:

- **`execve`**:加载 ELF 时,`load_elf_image` 给非可执行段(PT_LOAD 里没 `PF_X` 的)标 `FLAG_NX`(`elf_load.cpp:161`,`execve.cpp` 只调它、自身不标 NX)。
- **PF handler**:缺页时查到 VMA,如果这个 VMA 没有 `Exec` 权限(栈、heap、匿名页),给它补上 `FLAG_NX`(`page_fault.cpp:195`,匿名页路径)。文件 demand-read 也有同样的判断:io-mapped 页在 `page_fault.cpp:271`(ioflags)、普通文件页在 `page_fault.cpp:300`(fflags),非 `Exec` 就标 NX。于是一旦栈页被当代码执行,触发的是 instruction-fetch #PF(而非默默允许)。
- **`sys_mmap`**:匿名映射时,只要 `prot` 没带 `PROT_EXEC`,就标 `FLAG_NX`(`sys_mmap.cpp:318`)。

> **一个藏了很久的小矛盾,开 NXE 才消解。** `execve` 加载 ELF 时,早就给非可执行段(PT_LOAD 里没 `PF_X` 的)标了 `FLAG_NX`。但 NXE 没开时,bit-63 是 reserved 位——理论上设了该触发 reserved-bit #PF。测试一直绿,大概是这些数据段恰好没被当代码取指、没踩到。NXE 一开,bit-63 合法地解释成 NX,这个「设了 reserved 位却没炸」的矛盾就消了,设置从「理论危险实际侥幸」变成「正大光明」。开闸是安全的:内核自己的页从没标过 NX,开 NXE 不影响内核可执行。

`usermode_init_asm` 这个例程 BSP 和每个 AP 都走,所以改这一处,全核的 NXE 都设上——不用像 SMEP/SMAP 那样 per-CPU 各调一遍(虽然 EFER 严格说也 per-CPU,但启动路径统一走这个例程,一处覆盖)。

## SMEP:内核不许执行用户页(CR4[20],CPUID gate)

SMEP(Supervisor Mode Execution Prevention)堵的是「内核态执行用户态页面」。开法在 `paging.cpp`:

```cpp
void enable_smep_smap() {
    // CPUID.07H:EBX[7]=SMEP, [20]=SMAP (sub-leaf ecx=0)
    ...
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (cpuid_has_smep) cr4 |= (1ULL << 20);  // CR4.SMEP
    if (cpuid_has_smap) cr4 |= (1ULL << 21);  // CR4.SMAP
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
}
```

（`paging.cpp:47`,SMEP 在 `:53`、SMAP 在 `:56`。)注意那个 `if (cpuid_has_*)`——SMEP 必须先 CPUID 检测再写 CR4。为什么 NX 能无条件设、SMEP 不能?因为 SMEP 是 2011 年才加进 Intel 的特性,老 CPU 没有;往一个不支持的 CR4 位写 1,直接 #GP 崩。所以得先问 CPUID.07H:EBX 的 bit 7「你支持 SMEP 吗」,支持才设。QEMU 的 `qemu64` 和现代真机都支持,检测通过即设。

CR4 是 **per-CPU** 的——每个核有自己的 CR4。所以 `enable_smep_smap()` 得 BSP 调一次、每个 AP 再各调一次:BSP 在 `main.cpp:129`(`usermode_init()` 之后),每个 AP 在 `ap_main.cpp:147`(`usermode_init_asm()` 之后)。漏一个核,那个核就没保护。

SMEP 开了不会崩内核,是因为内核切到用户态走的是 `sysretq`/`iretq`(改 CS 切环),不是直接去执行用户页的代码;SMEP 只拦「Ring 0 执行 Ring 3 页」,不影响正常的环切换。

## SMAP:内核不许碰用户页(CR4[21] + stac/clac 全入口)

SMAP(Supervisor Mode Access Prevention)是这三个里的硬骨头。它堵的是「内核读写用户页」——比 SMEP 的「执行」更狠,连访问都拦。问题是:内核**本来就要**访问用户内存——读用户传来的指针参数、往用户缓冲写返回值。所以 SMAP 不能像 SMEP 那样「设上就完事」,得给所有「合法访问用户内存」的入口配一对 `stac`/`clac`:`stac`(置 AC flag)临时放行访问,`clac` 关上。开 SMAP 之前如果不把这对指令铺满,内核一碰用户内存就 #PF。

`stac`/`clac` 这对指令,现在的挂法是「合法访用户只走 accessor,入口不挂全局 stac」。早先的设计是在 SYSCALL/中断入口各挂一条全局 `stac`、出口挂 `clac`,后来(P3)发现这套「入口一刀切放行」太粗——任何系统调用/中断全程都放行用户访问,SMAP 等于在 handler 体里基本失效。于是把入口的 `stac` 撤掉,改成**只在真正需要碰用户内存的那个 accessor 里开窗**:`copy_from_user`/`copy_to_user`(`user_access.hpp:50`/`:52`)内部内联一条 `stac` 开窗、用完 `clac` 关上,窗口极小、且配合 `_ASM_EXTABLE` 容错(访用户失败走 fixup,fixup 里也 `clac` 再返回 `-EFAULT`)。SYSCALL 入口现在的 `stac` 是条注释,标记它已被移除(`syscall.S:67` `# stac (P3: global STAC removed -- SMAP real; user mem only via accessor stac)`);出口的 `clac` 还留着,作为「离开内核态前再确认 AC 是关的」这道防线(`syscall.S:185`):

```asm
# SYSCALL entry,swapgs 之后:
# stac  (P3: global STAC removed -- SMAP real; user mem only via accessor stac)
...                 # 保存现场、跑 C handler(期间只在 copy_from_user 内短暂 stac)
# exit:
clac                # 关上 AC,再 sysretq
sysretq
```

中断入口(`interrupts.S`)三个 ISR 宏(NOERRCODE/ERRCODE/IRQ)也同理:入口的 `stac` 现在是注释行(`:75`/`:191`/`:307`,标记 P3 移除),出口的 `clac` 还挂在三个 IRETQ 前(`:116`/`:232`/`:354`)。原来「只在从用户态进来的分支才 `stac`」的那段条件逻辑也跟着入口 `stac` 一起撤了——既然入口不再开窗,就没有「是否从用户态进来」之分;`clac` 留在出口纯粹是「出内核态前再关一次 AC」的兜底。

```asm
# exit:
clac                # F9 batch 4: SMAP -- re-forbid user access before IRETQ
...
iretq
```

为什么出口还留 `clac`?因为 accessor 的 `stac` 是在 handler 体里开的极小窗口,正常路径 accessor 自己 `clac` 关上了。但如果 handler 出了什么岔子(异常、提前返回)没关干净,出口这条 `clac` 兜底,保证回到用户态或回到中断前现场时 AC 是关的——SMAP 不被一个没关严的窗口长期放行。入口不挂 `stac`,意味着「凡是没显式走 accessor 的访用户,一律被 SMAP 当非法访用户 #PF 拦下」,这正是 SMAP 想要的纪律。

> **两步走,把风险拆开。** 这套 `stac`/`clac` 是在开 SMAP **之前**就加进去的。关键性质:`stac`/`clac` 在 SMAP 没开时是**无害的 NOP**(AC flag 在 SMAP 关时不影响访问)。所以可以先加指令、跑测试确认 asm 写对了(行为不变,931 全绿),再开 SMAP、跑测试确认覆盖全(没漏哪个访用户入口)。两件事分开验证——asm 对不对 / SMAP 覆盖全不全——比一锅端安全得多。这是处理「先铺基础设施再开总闸」这类改动的通用套路。

## 怎么验证一个「开了但不一定生效」的机制

这三个位开了之后,怎么知道真生效?最直白的办法是「故意触发一次攻击看它拦没拦」——比如往 NX 的栈页里塞 shellcode 然后跳过去,看是不是 #PF。但这在测试里又难造又脆。Cinux 走的是**机制回读**:直接读 EFER 和 CR4,看那几个位是不是按预期设上了。这在 `test_usermode.cpp` 的 `test_f9_nxe_smep_smap_enabled`:

```cpp
void test_f9_nxe_smep_smap_enabled() {
    // F9: EFER.NXE (bit 11) is x86_64 baseline -- always expected on.
    ...  // 读 EFER,断言 bit 11 = 1

    // SMEP/SMAP are CPUID-gated (CPUID.07H:EBX[7]/[20]). The test mirrors
    // enable_smep_smap(): CPU 报支持才断言 CR4 位设上。
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    ...  // CPUID 查 SMEP/SMAP 支持,支持才断言 CR4[20]/[21]
}
```

（`test_usermode.cpp:122`。)NXE 是 baseline,**无条件断言**——任何环境都该设上。SMEP/SMAP **跟随 CPUID**:CPU 报支持(`-cpu host`,真机暴露 SMEP/SMAP)才断言 CR4 位设上;CPU 不报支持(WSL2 的 `-cpu max`,CPUID.07H:EBX=0)就不断言、也不断错。测试和 `enable_smep_smap()` 用同一套 CPUID 判据,逻辑自洽。

这里有个写测试的坑值得记一笔:第一版断言写成 `TEST_ASSERT_TRUE(efer & 0x800)`。`efer & 0x800` 的结果是 `0x800`(不是 0/1),如果断言宏按 `== true`(==1)判,就**假阴性 FAIL**。改成 `(efer >> 11) & 1`(结果恒为 0/1)才稳。位测试一律用「移位再 `& 1`」取 0/1 形式,别直接拿掩码与的结果当布尔。

## 诚实的边界

**SMEP/SMAP 在 WSL2 开发机上验证不了生效。** 这是环境限制,不是代码问题。WSL2 的嵌套 KVM 用 `-cpu max` 时,不透传 CPUID.07H:EBX(整个 leaf 7 返回 0),所以 `enable_smep_smap()` 的 CPUID-gated 逻辑**正确地跳过**了 SMEP/SMAP——往不支持的位写会 #GP,跳过是对的。表现就是:开发机上 EFER.NXE 设上了(NX 真生效),CR4 的 SMEP/SMAP 没设(代码 CPUID-gated 跳过)。`stac`/`clac` 此时是 NOP,无害。换真机、完整(非嵌套)KVM、或 QEMU TCG,CPUID.07H 正常暴露,SMEP/SMAP 就设上、真生效。所以别在本机上指望看到「SMEP 拦了一次内核执行用户页」——看不到不是没做对,是环境没给条件。

**NX 是真能在本机验的。** EFER.NXE 是 x86_64 baseline,WSL2 透传,设上了就是真生效:用户栈/heap/非可执行文件页不可执行,真要执行会 instruction-fetch #PF。这是三个位里唯一在本机板上钉钉的那个。

**SMAP 开了之后,「访用户内存」的纪律更严。** 入口已经不挂全局 `stac`(P3 移除),所以凡是没显式走 accessor(`copy_from_user`/`copy_to_user`,内部 `stac` 开窗 + `_ASM_EXTABLE` 容错)的访用户,一律被 SMAP 当非法访用户 #PF 拦下——包括 `validate_user_ptr` 那种只查 canonical 地址就直接解引用的旧路径。SMAP 让这条边界变成「不开窗就碰不得」,比之前 PF 兜底默默通过更安全。

验证该看到什么,见配套 lab。下一章(056b)接着开 ASLR——给用户态布局加随机化,那是 F9 安全的另一条线。
