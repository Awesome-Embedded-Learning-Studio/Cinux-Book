---
title: Lab 022 · 第一次跳进 Ring 3:用户态与特权隔离
---

# Lab 022 · 第一次跳进 Ring 3:用户态与特权隔离

> 配套章节:[022 · 第一次跳进 Ring 3:用户态与特权隔离](../../book/07-userland/022-ring3-usermode.md)。这一关给你目标和约束,不贴 `usermode_init_asm` 的 MSR 写入全文、不贴 `jump_to_usermode` 的 SYSRET 汇编、不贴 `launch_first_user()` 的分步编排——那些得你自己写、自己踩坑、自己修出来。最后那条 `cli` 在 Ring 3 撞出来的 `#GP`,是隔离成立的唯一证据。

## 实验目标

把内核从「只有 Ring 0」推进到「能真正跳进 Ring 3,并被特权隔离弹回来」。这一关的形态很特别:它**不是**一个能跑业务逻辑的用户态,而是一次「单向跳进去 + 撞墙」的演示。用户代码是硬编码在内核里的四字节(`cli; hlt; jmp .-2`),第一条指令就触发 `#GP` 然后 halt,不回到调度器、不跟内核说话。把边界想清楚,后面才不会过度承诺。拆成几个能独立验证的子目标:

1. **装配 SYSRET 的三只 MSR**:写 `STAR(0xC0000081)` 把 `[63:48]`/`[47:32]` 都填成内核 CS `0x08`;写 `SFMASK(0xC0000084)` 填 `0x200`;读改写 `EFER(0xC0000080)` 置 `SCE` 位。
2. **写 SYSRET 的跳板**:按硬件约定摆好寄存器(`rcx=entry`、`rsp=user_stack`、`r11=0x202`、其余清零),最后一条 `sysretq` 进 Ring 3。
3. **给 GDT 配齐用户态所需的 TSS**:一块 4 KB 的 Double Fault 栈挂进 `tss_.ist[0]`,IDT 把 `#DF` 挂到 `IST1`;再提供一条写 `tss_.rsp[0]` 的入口——这是 Ring 3 触发异常时 CPU 自动切回内核栈的着陆点。
4. **搭一个能跑 Ring 3 的用户地址空间**:新建 `AddressSpace`,在 `USER_ENTRY_BASE` 映一页代码、在 `USER_STACK_TOP` 下面映 4 页栈,页标志带上 `FLAG_USER`;激活前把内核 PDPT 里 framebuffer 用的 1 GB identity-mapping 抄进用户 PDPT;激活后设好 `TSS.RSP0`,再 `jump_to_usermode`。
5. **修 `walk_level` 的 user 位**:四级页表每一级的中间页表项都得带 `FLAG_USER`,否则 Ring 3 一访问就 `#PF`,error code `0x05`。
6. **让 `handle_gp` 认出用户来源**:`(frame->cs & 0x03) != 0` 就是 Ring 3 来的,打出 `protection works`。

做完这几条,内核就第一次有了「Ring 0 / Ring 3 之间那堵墙」。但这一关里,用户态没有任何合法方式回到内核——没有 syscall、没有 ELF、没有调度器回环。下一站才会把那扇门开出来。

## 前置条件

你得先过 Lab 009(GDT/IDT)、015(PMM)、016(VMM)、018(AddressSpace)。关键依赖:

- **009 的 GDT/IDT**:GDT 里早就有 user code(`0x1B`)/user data(`0x23`)/TSS(`0x28`)这几个选择子常量;IDT 的异常路由表(向量 0–14)也在。这一关要往这两处「加料」——给 TSS 配 IST1 栈、给路由表加 `ist` 字段。
- **015 的 `g_pmm.alloc_page()`**:用户代码页、4 页用户栈、TSS 的 Double Fault 栈,物理页都从这儿要。
- **016 的 `g_vmm.map(virt, phys, flags, uint64_t* pml4)`**:它带个可选的 `pml4` 根参数,这一关终于用上了——用户地址空间有自己的 PML4 根。而且本关要改它的实现(给 `walk_level` 加 `user_flag`)。
- **018 的 `AddressSpace`**:构造时只把内核 PML4 的高半区(`PML4[256..511]`)复制过去,低半区全清零——`activate()` 切 CR3、`pml4_phys()` 取根物理地址。本关的「framebuffer identity-mapping 丢失」那个坑,根因就长在这条设计上。

还得吃透两个外部约定。第一,**SYSRET 的硬件契约**(Intel SDM Vol.3A §5.8.8):`RIP ← RCX`、`RFLAGS ← R11`、`CS = STAR[63:48]+16`、`SS = STAR[63:48]+8`,而且 **SYSRET 不修改 RSP**——栈得软件自己切。第二,**`wrmsr` 只写 `EDX:EAX`**:64 位 RDX 的高 32 位会被丢掉。这两条是这一关两个最容易翻车的地方的根。

最后一条容易被头注释误导的话先说在前面:`main.cpp` 头注释里写着「Step 17. Scheduler init, create tasks / 18. Launch first user-mode program」,看起来像「调度器 + 用户态并存」。在本 tag 的代码里,Step 17 的 Scheduler init **已经被删掉了**(diff 里 021 的 producer/consumer、`Scheduler::init`、`run_first` 整段都没了),实际只剩 `usermode_init()` + `launch_first_user()`。注释是没擦干净的旧文本,别照着写实现。

## 任务分解

**第一步:`usermode_init_asm`,写三只 MSR。** 函数无入参、无返回,只 clobber `%rax`/`%rcx`/`%rdx`。三段:`wrmsr` 写 STAR、写 SFMASK、读改写 EFER。

STAR 这段是这一关的「为什么不能偷懒」第一课。你想把 `0x08` 同时塞进 `STAR[63:48]`(SYSRET 用)和 `STAR[47:32]`(SYSCALL 用)。一个自然的写法是 `movq $0x08, %rdx; shlq $32, %rdx; orq $0x08, %rdx`——直觉上「左移 32 位把 0x08 放到高 32 位,再 OR 一个低位的 0x08」。但这一行会让你进不了 Ring 3。根因在 `wrmsr` 的语义:它只写 `EDX:EAX`,而 `EDX` 是 `RDX` 的**低 32 位**。你 `shlq $32` 之后那个 `0x08` 落在 `RDX[47:32]`,完全在 `EDX` 之外,被硬件丢弃,于是 `STAR[63:48] = 0x00`,SYSRET 算出来的 CS 不是 `0x1B` 而是个数据段选择子。

正确的移位是 `shlq $16`——让 `0x08` 落在 `EDX[31:16]`,正好对应 `STAR[63:48]`。然后再 `orq $0x08` 把 `EDX[15:0]` 填上,对应 `STAR[47:32]`。最后 `%rax`(对应 STAR 低 32 位)清零。写完之后,你可以算一下期望值:`EDX:EAX = 0x00080008_00000000`,即 STAR 高 32 位 `0x00080008`。这一段**不给汇编全文**,但给你这条不可破的约束:**移位量必须是 `$16` 不是 `$32`,理由就是 `wrmsr` 只认 32 位 `EDX`**。

SFMASK 这段写 `0x200`(屏蔽 IF 位),看起来正经,但它在本关**写了也白写**——见后面「常见故障」里的 QEMU 那条,这里先记住:它的作用域只覆盖 SYSCALL 方向,SYSRET 从 `R11` 恢复 RFLAGS,根本不读 SFMASK。所以这一关它写不写、写成多少,对「跳进 Ring 3」都没有功能影响。

EFER 这段是读改写:`rdmsr` 读 `EFER(0xC0000080)` 到 `%edx:%eax`,`orq $1, %rax` 置 `SCE` 位(bit 0),再 `wrmsr` 写回。SCE 是「System Call Extensions」的总开关,不开它 `sysretq` 直接 `#UD`。这一段没坑,但别忘了 `rdmsr` 之后 `%edx` 已经是 EFER 的高 32 位,你只改 `%eax` 再 `wrmsr`,高半区原样保留——别手贱 `xor` 掉 `%edx`。

**第二步:`jump_to_usermode`,SYSRET 的跳板。** System V 约定下,`%rdi = entry`、`%rsi = user_stack`、`%rdx = arg`。按 SYSRET 契约摆寄存器:`mov %rdi, %rcx`(RCX → RIP)、`mov %rsi, %rsp`(软件切栈,SYSRET 不动 RSP)、`mov %rdx, %rdi`(用户入口的第一个参数)。然后构造 RFLAGS:`pushq $0x202; popq %r11`——`0x202` 是 IF(bit 9,开中断)+ bit 1(RFLAGS 里永远为 1 的保留位)。接着把其余 GPR 全部 `xor` 清零,目的是**不让内核寄存器里的值泄漏进用户态**(RAX/RBX 等可能残留内核指针)。最后一条 `sysretq`。

这一段同样不给汇编全文,但有四条约束。其一,**`%rcx` 装的是 entry,SYSRET 会把它塞进 RIP**——别和「第一个参数走 `%rdi`」搞混,你在 `mov %rdx, %rdi` 之后才把 `%rcx` 准备好,顺序无所谓,但语义上 `%rcx` 是返回地址槽、`%rdi` 是用户函数参数槽,两件事。其二,**`%rsp` 必须在 `sysretq` 之前切到用户栈**,因为 SYSRET 硬件不碰 RSP。其三,**`%r11` 必须是 `0x202` 这种合法 RFLAGS 值**,如果你忘了 pop 这一步、`%r11` 是随机值,SYSRET 之后 RFLAGS 会乱,IF 可能没开,中断进不来。其四,**清 GPR 不能漏 `%rcx`/`%r11` 之外的所有寄存器**——漏一个,内核数据就漏进用户态,这关虽然没有真用户程序来读它,但养成习惯。

**第三步:GDT 加 TSS 的料(`gdt.hpp`/`gdt.cpp`)。** 你要做的不是改用户段选择子(`0x1B`/`0x23` 早就有了),而是给 TSS 配一块独立栈,并提供写 RSP0 的入口。

具体说:在 `GDT` 类里加一个 `alignas(16) uint8_t df_stack_[4096]`(1 页,4 KB),在 `GDT::init()` 里把 `tss_.ist[0]` 设成 `&df_stack_[sizeof(df_stack_)]`(栈顶,因为栈往下长)。`TaskStateSegment` 结构是 104 字节(`static_assert(sizeof(TaskStateSegment) == 104)` 锁死),字段顺序:`reserved0 / rsp[3] / reserved1 / ist[7] / reserved2 / reserved3 / iomap_base`。`ist[0]` 就是 `IST1`(注意编号差一:IST1 对应数组下标 0)。再加一个静态方法 `GDT::tss_set_rsp0(uint64_t rsp0)`,它写 `g_gdt.tss_.rsp[0]`——`rsp[0]` 就是 RSP0,即 `tss_` 里偏移 4 的那个字段。

为什么 RSP0 这么重要?Ring 3 一旦触发异常(比如我们故意触发的 `#GP`),CPU 要从用户栈切回内核栈,它去哪儿找内核栈?**看 `TSS.RSP0`**。如果你没设它、或设错了,异常发生时 CPU 在一个悬空的栈上压帧,下一次就 Triple Fault。这一关的 demo 直接用「当前 `%rsp`」当 RSP0(因为 `launch_first_user` 不返回、也不做任务切换,当前内核栈就是着陆栈),这是这一关能偷的懒,别学进真调度器里。

**第四步:IDT 路由表加 `ist` 字段(`idt.cpp`)。** 之前的路由表是 4 元组 `{vector, stub, priv, gate}`,本关升级成 5 元组,多一个 `ist`。`IDT::set_handler` 多收一个 `uint8_t ist` 参数,写进 IDT 条目的 `ist` 字段(它在门描述符里的位置是 bits[34:32],三位的 IST 索引)。路由表里只有 `#DF`(vector 8)挂 `ist=1`,其余 `ist=0`。

为什么要给 `#DF` 单独的栈?Double Fault 的定义就是「处理一个异常时又触发了异常」——最典型的是 page fault handler 自己又缺页。如果 `#DF` 还在原来那个已经烂掉的栈上跑,就稳稳 Triple Fault、机器重启。`IST1` 机制让 `#DF` 不管当前栈在哪、不管 CPL 是几,都强行切到 `tss_.ist[0]` 指向的那块独立栈上跑。这就是你在第三步准备那块 `df_stack_` 的用途。

**第五步:`launch_first_user` 的分步编排(`usermode.cpp`)。** 这是这一关的「主舞台」,但**不给完整实现**。按顺序你要排的是:

- 建一个 `AddressSpace user_space`。
- `g_pmm.alloc_page()` 要一页物理,`user_space.map(USER_ENTRY_BASE, code_phys, kUserPageFlags)` 映成代码页。`USER_ENTRY_BASE = 0x400000`,`kUserPageFlags = FLAG_PRESENT | FLAG_WRITABLE | FLAG_USER`。
- 把那四字节字节流(`kUserCode[] = {0xFA, 0xF4, 0xEB, 0xFC}`)写进代码页。这里有个细节:你要通过内核的 higher-half direct map 去写那页物理,而不是直接解引用物理地址——`code_phys + KERNEL_VMA`(`KERNEL_VMA = 0xFFFFFFFF80000000`)得到内核视角的虚拟地址,逐字节拷。
- 在 `USER_STACK_TOP = 0x7FFFFF000` 下面映 `USER_STACK_PAGES = 4` 页栈。栈往下长,所以栈基址是 `USER_STACK_TOP - 4*4096 = 0x7FFFFB000`,从栈基址往上逐页 `map`。每页都要带 `FLAG_USER`。
- **关键一步**:在 `activate()` 之前,把内核 PDPT 里的 identity-mapping 条目抄进用户 PDPT。下面单独讲。
- `user_space.activate()` 切 CR3,打日志说 PML4 物理地址。
- `movq %rsp, kernel_rsp0` 取当前栈顶,`GDT::tss_set_rsp0(kernel_rsp0)` 设进 TSS。
- `jump_to_usermode(USER_ENTRY_BASE, USER_STACK_TOP, 0)`。调用之后**不会返回**(用户代码第一条 `cli` 触发 `#GP` → `fatal_halt`)。

为什么抄 PDPT 这步单独拎出来?因为 framebuffer 用的是 identity mapping——物理地址直接当虚拟地址用,靠一张 1 GB 大页挂在 `PDPT[3]`。而 `AddressSpace` 构造时**只复制高半区**(`PML4[256..511]`),低半区全清零。你一 `activate()`,CR3 换成用户 PML4,那张 1 GB 大页就消失了。接着 `kprintf` 想往 console 写 → framebuffer 地址缺页 → demand-page handler 顺手给它映了一页普通 RAM → 写进去啥也没有。更糟的是 page fault handler 自己又调 `kprintf` 想打日志,再次缺页,形成**重入**,串口就开始吐乱码。

修复的做法:激活前遍历内核 PDPT 的 512 个条目,凡是内核有、用户没有的,抄过去。注意只抄「缺失」的(`kern_pdpt[i] present && !user_pdpt[i] present`),别覆盖你已经映好的用户页。代码不长,但**不给成品**——你自己写,关键想清楚为什么是抄 PDPT 这一级而不是 PML4 这一级(答:1 GB 大页就挂在 PDPT,复制 PML4 条目只是复制了「PDPT 在哪」这个指针,PDPT 本身的内容还是用户的)。

**第六步:`walk_level` 加 `user_flag`(`vmm.cpp`)。** 这是「为什么不能偷懒」第二课。x86-64 是四级页表(PML4→PDPT→PD→PT),Ring 3 访问一页时,CPU 会**逐级**检查 user 位(bit 2)。只要任何一级的页表项没带 `FLAG_USER`,整个访问就被拒,触发 `#PF`,error code `0x05`(P=1 页存在、W/R=0 读、U/S=1 用户——权限不足,不是缺页)。

原来的 `walk_level` 在分配新的 PDPT/PD/PT 页时,页表项只设了 `FLAG_PRESENT | FLAG_WRITABLE`,漏了 `FLAG_USER`。于是哪怕你最终在 PT 那一级把页映上了 `FLAG_USER`,中间某一级(PDPT 或 PD)是「内核专用」的,Ring 3 照样被拒。这一坑尤其阴——因为它和「framebuffer 丢失」的 `#PF` 症状很像,容易误判。

修复:给 `walk_level` 加一个 `uint64_t user_flag = 0` 参数(默认 0,保持内核映射不受影响),在 `VMM::map()` 入口处从 `flags` 里抠出 `FLAG_USER`(`uint64_t user_flag = flags & FLAG_USER`),一路传给四级 `walk_level` 调用。`walk_level` 里凡是为新页表项写 `entry.raw` 的地方,都 `| user_flag`。改完之后,从 `AddressSpace::map` 到最底层 PT 的每一级,只要最终目标页是用户页,中间层级也全是用户页。这一段不给完整 diff,但给你这条约束:**`user_flag` 必须从 `VMM::map` 的 `flags` 提取,然后四级 `walk_level` 全部传同一份**,任何一级漏传就是 `#PF 0x05`。

**第七步:`handle_gp` 区分来源(`exception_handlers.cpp`)。** 在 `handle_gp(InterruptFrame* frame)` 里加一行判断:`bool from_user = (frame->cs & 0x03) != 0;`。`cs` 的低两位是 RPL(Requested Privilege Level),Ring 3 来的异常 `cs` 会是 `0x1B`(RPL=3),内核自己触发的 `#GP` `cs` 是 `0x08`(RPL=0)。来自用户的就打 `#GP at RIP=... from user mode (Ring 3)` + `Privileged instruction executed in Ring 3 -- protection works!`。然后照例 `fatal_halt()`。这一行就是 milestone 的验收信号——串口看到 `protection works`,隔离就成立了。

## 接口约束

你要实现或修改出来的东西,对外长这样(职责与签名,不给实现):

- `namespace cinux::arch` 下:
  - `constexpr uint64_t USER_ENTRY_BASE = 0x400000;`
  - `constexpr uint64_t USER_STACK_TOP = 0x7FFFFF000;`
  - `constexpr uint64_t USER_STACK_PAGES = 4;`
  - `void usermode_init();`——调用汇编 `usermode_init_asm()` 写三只 MSR,打一行 `[USER] STAR/EFER MSRs configured for SYSRET.`。
  - `void launch_first_user();`——分步编排,调用后不返回(注释里写明 `@note This function does not return in the normal sense.`)。
- `extern "C" void jump_to_usermode(uint64_t entry, uint64_t user_stack, uint64_t arg);`——汇编实现,SYSRET 跳进 Ring 3,「不返回」。
- `extern "C" void usermode_init_asm();`——汇编实现,写 STAR/SFMASK/EFER。
- `class GDT`:
  - `static void tss_set_rsp0(uint64_t rsp0);`——写 `g_gdt.tss_.rsp[0]`。
  - 私有:`alignas(16) uint8_t df_stack_[4096];`、`TaskStateSegment tss_;`(`static_assert(sizeof == 104)`)、`GDT::init()` 里设 `tss_.ist[0]`。
- `class IDT` / 路由表:`set_handler(ExceptionVector, Stub, uint16_t selector, uint8_t type_attr, uint8_t ist)` 多收 `ist`;`#DF` 挂 `ist=1`,其余 `ist=0`。
- `void handle_gp(InterruptFrame* frame);`——`(frame->cs & 0x03) != 0` 区分用户来源。
- `VMM::map` 内部:`walk_level` 加 `uint64_t user_flag = 0` 参数,`VMM::map` 提取 `flags & FLAG_USER` 向下传四级。

关键约束(违反就翻车):

- **STAR 写入移位量必须是 `shlq $16`,不是 `$32`**。理由:`wrmsr` 只写 `EDX:EAX`,`EDX` 是 RDX 的低 32 位;`shlq $32` 把值推到 RDX 高 32 位,被硬件丢弃,`STAR[63:48]` 变 0,SYSRET 后 CS 不是 `0x1B` 而是数据段选择子。`shlq $16` 让值落在 `EDX[31:16]`。
- **`sysretq` 之前必须软件切好 `%rsp`**。SYSRET 硬件不修改 RSP。忘切就直接在内核栈上「跑用户代码」,特权没真切换,后面一切症状都对不上。
- **`%r11` 必须是合法 RFLAGS 值(本关 `0x202`)**。SYSRET 把 `R11` 装进 RFLAGS。漏 pop 或写成随机值,IF 可能没开,用户态收不到中断(本关 demo 不依赖中断,但别埋雷)。
- **四级页表的每一级都要带 `FLAG_USER`**。`walk_level` 的 `user_flag` 必须从 `VMM::map` 的 `flags` 一路传到 PDPT/PD/PT。任何一级漏 → `#PF 0x05`(P=1,W/R=0,U/S=1,权限不足)。
- **`launch_first_user` 在 `activate()` 之前必须抄内核 PDPT 的 identity-mapping 条目进用户 PDPT**。不抄 → 切 CR3 后 framebuffer 的 1 GB 大页消失 → `kprintf` 缺页重入 → 串口乱码。
- **设 `TSS.RSP0` 必须在 `activate()` 之后、`jump_to_usermode` 之前**。RSP0 是 Ring 3 异常回内核的着陆栈;没设或设错,`#GP` 时 CPU 在悬空栈上压帧,下一步 Triple Fault。
- **`launch_first_user` 不返回**。用户代码第一条 `cli` 触发 `#GP` → `fatal_halt` 永久 `cli; hlt`。`main.cpp` 里它后面的 `Returned from user mode launch (unexpected)` 和键盘 poll loop 在本 demo **不可达**,别为了让它「正常返回」去改 SYSRET。

移位具体写几条 `mov`/`shl`/`or`、PDPT 抄录的循环边界、`kUserCode` 数组怎么 `memcpy` 进去、`walk_level` 内部几个 `entry.raw` 赋值——这些**这一关不提供**,你自己写,但写下来要和上面的约束、和 host 测试里的算术对得上。

## 验证步骤

常量和纯算术(用户态常量、STAR 计算、TSS 布局、字节码、页标志)在 host 上测,不链内核、不跑汇编,`CINUX_HOST_TEST` 门控。建议确认覆盖:`USER_ENTRY_BASE = 0x400000`、`USER_STACK_TOP = 0x7FFFFF000`、`USER_STACK_PAGES = 4`、栈基址 `0x7FFFFB000`、字节码 `cli=0xFA`/`hlt=0xF4`/`jmp -4 = EB FC`、STAR 高 32 位 `0x00080008`、SYSRET 推出的 `CS = 0x08+16+3 = 0x1B`、`SS = (0x08+8)|3 = 0x13`、RFLAGS `0x202`、用户页标志 `0x7`、镜像 `TestTSS`(`sizeof=104`、`ist@36`、`iomap_base@102`)、`(cs & 0x03)` 判用户态:

```bash
ctest --test-dir build -R usermode --output-on-failure
```

真正的 MSR/GDT/TSS/AddressSpace(真汇编、真 wrmsr、真 CR3)只能在 QEMU 里验。机内测要覆盖:`TSS.RSP0` 写入不崩、STAR 读回 `STAR[63:48] = 0x08` 且 `STAR[47:32] = 0x08`、`EFER.SCE = 1`、SFMASK 验「写 `0x200` 不触发 `#GP`」(**不断言读回值**,见常见故障)、用户 `AddressSpace` 创建/map/translate/隔离、段选择子 inline asm(`mov %cs/%ds/%ss`、`str`)、`#DF` 的 IST1 配置、`usermode_init` 已调用:

```bash
cmake --build build --target run-big-kernel-test
```

机内会打节头 `Usermode Tests (022)`,末尾 `ALL TESTS PASSED`。

最后跑**生产 demo** 本身(直接跑大内核的 QEMU 目标),串口应看到:

```text
[USER] STAR/EFER MSRs configured for SYSRET.
[BIG] ===== Milestone 022: User Mode (Ring 3) =====
[USER] Setting up first user-mode program...
[USER] User address space activated (PML4 at phys ...).
[USER] Jumping to Ring 3: entry=0x0000000000400000 stack=0x00000007FFFFF000

==== EXCEPTION: #GP (vector 13) ====
  RIP   = 0x0000000000400000   CS  = 0x001b
  ...
[EXCEPTION] #GP at RIP=0x0000000000400000 from user mode (Ring 3)
[EXCEPTION] Privileged instruction executed in Ring 3 -- protection works!
```

`CS = 0x001b`(用户代码段,Ring 3)+ `protection works` 两条同时出现,隔离就成立了。`RIP = 0x400000` 正是 `USER_ENTRY_BASE`,说明用户代码真的从那条 `cli` 开跑、第一拍就被 `#GP` 拦下。

## 常见故障

- **串口狂吐 `[[[[[[[[[`,夹杂 `VMM Demand-paged` 日志,越吐越乱**:framebuffer 的 identity-mapping 在用户地址空间里丢了。`AddressSpace` 只复制高半区,切 CR3 后那张 1 GB 大页消失,`kprintf` 缺页,demand-page handler 给它映了普通 RAM 页,handler 内部又 `kprintf` 再次缺页,形成重入。修复:`launch_first_user` 在 `activate()` 之前,遍历内核 PDPT,把内核有、用户没有的条目抄进用户 PDPT。
- **修了 framebuffer 还是进不去,而且崩在 `RIP = 0x400000` 但 `CS = 0x13`**:STAR 写入用了 `shlq $32`。`wrmsr` 只写 `EDX:EAX`,`shlq $32` 把 `0x08` 推到 RDX 高 32 位被丢弃,`STAR[63:48] = 0`,SYSRET 算出 `CS = 0x13`(数据段选择子 + RPL3),CPU 在数据段上取指。修复:`shlq $16`,让值落 `EDX[31:16]`。这个 bug 经常被 framebuffer 那个 `#PF` 掩盖,先修 PDPT 抄录才会暴露它。
- **framebuffer 和 STAR 都修了,Ring 3 一访问代码页就 `#PF`,error code `0x05`**:`walk_level` 分配的中间页表项只设了 `PRESENT | WRITABLE`,漏 `FLAG_USER`。四级页表任何一级缺 user 位,Ring 3 访问被拒。`error_code = 0x05` 是「页存在但权限不足」(P=1,W/R=0,U/S=1)的指纹。修复:`walk_level` 加 `user_flag` 参数,从 `VMM::map` 的 `flags` 提取 `FLAG_USER`,四级调用全部传同一份。
- **SYSRET 之后异常没回来,或者回来的栈是乱的**:忘了在 `sysretq` 之前 `mov %rsi, %rsp` 切栈。SYSRET 硬件不碰 RSP,你直接在内核栈上「跑用户代码」,特权切换语义全乱。修复:`jump_to_usermode` 里先切 `%rsp` 再 `sysretq`。
- **`#GP` 触发后立刻 Triple Fault、机器重启**:没设 `TSS.RSP0`,或设错了。Ring 3 异常回内核时 CPU 看 RSP0 找内核栈,没设就在悬空栈上压帧,下一步 Triple Fault。修复:`activate()` 之后、`jump_to_usermode` 之前,`movq %rsp, kernel_rsp0` + `GDT::tss_set_rsp0(kernel_rsp0)`。
- **`sysretq` 直接 `#UD`**:EFER.SCE 没开。`usermode_init` 里 `EFER` 读改写那段漏了 `orq $1, %rax`,或 `wrmsr` 写回之前手贱 `xor` 掉了 `%edx`。修复:只改 `%eax`(SCE 在 bit 0),`%edx`(EFER 高 32 位)原样写回。
- **`test_sfmask_if_bit` 失败,`(sfmask & 0x200) == true` 不成立**:**这不是你的 bug,是 QEMU 的**。QEMU(KVM 和 TCG 两种后端都一样)对 `IA32_FMASK` 的合法写入**静默丢弃**——`wrmsr` 不触发 `#GP`(值合法),但 `rdmsr` 读回是 0。验证方法:写全 1(`0xFFFFFFFF:0xFFFFFFFF`)会正常触发 `#GP`,证明你的指令编码没错;只是合法值被模拟器丢了。修复:把测试从「硬断言读回值」改成「写 `0x200` 不触发 `#GP` 即通过」。真硬件上应读回 `0x200`。顺便记住:SFMASK 只影响 SYSCALL 方向,本关只用 SYSRET、从 `R11` 恢复 RFLAGS,这值在本关本来就无功能影响。

## 通过标准

1. `usermode_init_asm` 正确写三只 MSR:STAR 高 32 位 `0x00080008`(`shlq $16`,不是 `$32`)、SFMASK 写 `0x200`(允许被 QEMU 丢)、EFER 置 SCE 位(只改 `%eax`、保留 `%edx`)。
2. `jump_to_usermode` 按 SYSRET 契约摆寄存器:`%rcx=entry`、软件切 `%rsp=user_stack`、`%r11=0x202`、清干净其余 GPR,末尾 `sysretq`。
3. GDT 给 TSS 配齐:`df_stack_[4096]` 挂 `tss_.ist[0]`;`TaskStateSegment` 104 字节、字段偏移用 `static_assert` 锁死;`GDT::tss_set_rsp0` 写 `rsp[0]`。IDT 路由表 5 元组化,`#DF` 挂 `ist=1`。
4. `launch_first_user` 分步编排:建 `AddressSpace`、映代码页(带 `FLAG_USER`)、写字节流、映 4 页栈、**抄内核 PDPT identity-mapping 进用户 PDPT**、`activate()`、设 `TSS.RSP0`、`jump_to_usermode`。函数不返回。
5. `walk_level` 加 `user_flag` 参数,从 `VMM::map` 的 `flags` 提取 `FLAG_USER` 向下传四级;四级页表任一级都不缺 user 位。
6. `handle_gp` 用 `(frame->cs & 0x03) != 0` 区分来源,用户来源打 `protection works`。
7. host 单测全绿(常量/STAR 计算/TSS 布局/字节码/页标志);QEMU 机内测节 `Usermode Tests (022)` 全过;生产 demo 串口见 `CS=0x001b` + `#GP ... from user mode` + `protection works`。

做到这七条,内核就第一次有了 Ring 3,也有了「用户撞特权墙」的证据。但这一关里,用户态没有任何合法方式跟内核通信——`cli` 是「非法」的,所以它只能触发异常。下一站 023 接 SYSCALL:装 syscall 入口、`sys_write`/`sys_exit`/`sys_yield`、user libc、从磁盘加载的真 ELF 用户程序——让 Ring 3 能「合法地」请求内核做事,而不是只能撞墙。那一关还会带来 `swapgs`、GS base 切换,以及「用户程序是个独立 ELF」这一整条加载链路。
