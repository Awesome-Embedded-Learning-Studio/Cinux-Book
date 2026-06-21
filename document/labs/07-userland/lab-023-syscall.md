---
title: Lab 023 · 让用户态会说话:SYSCALL/SYSRET 系统调用
---

# Lab 023 · 让用户态会说话:SYSCALL/SYSRET 系统调用

> 配套章节:[023 · 让用户态会说话:SYSCALL/SYSRET 系统调用](../../book/07-userland/023-syscall.md)。这一关给你目标和约束,不贴 `syscall.S` 的 134 行汇编、不贴 `build()` 三步的成品脚本、不贴 trap frame 那一段 push 序列、不贴「第 7 参压栈后偏移 +8」那条重排的完整指令——那些得你一条条敲出来,自己把六参挪位、自己把返回值绕道 `%rbx`、自己在 QEMU 里把 `movaps #GP` 和「169 个测试全跳过」的坑踩一遍。

## 实验目标

022 已经能让一段代码跑进 Ring 3 了,可那段代码是手写的 4 字节机器码(`cli;hlt;jmp .-2`),除了证明「特权指令在 Ring 3 会触发 #GP」之外什么都干不了。内核和用户之间没有一条「像函数调用一样的受控通道」——用户想往屏幕打一行字,都无处下嘴。这一关要打通的就是这条通道:用户程序执行 `syscall` 请求内核干活,内核处理完用 `sysretq` 把它送回 Ring 3。

拆成五件能独立验证的事:

1. **配三只 MSR**:`STAR`(SYSCALL/SYSRET 各取哪一段的段选择子)、`LSTAR`(入口地址)、`SFMASK`(入口清掉哪些 RFLAGS 位)。让 `syscall` 指令知道往哪跳、用什么段、要不要清 IF。
2. **写 `syscall_entry` 汇编**:入口 `swapgs` 换内核 GS、用 `%gs:8/%gs:0` 切内核栈、在内核栈上按固定顺序搭一个 12 槽 trap frame、把第 7 个参数挪到栈上凑 SysV 调用约定、`call` C 的 `syscall_dispatch`、返回值经 `%rbx` 中转、恢复现场、`sysretq` 回 Ring 3。这是这一关的灵魂。
3. **搭 dispatch 表**:`SyscallNr` 枚举(对齐 Linux 的 `SYS_read=0/SYS_write=1/SYS_yield=24/SYS_exit=60`)、`SYSCALL_TABLE_SIZE=256`、`syscall_register` 填表、`syscall_dispatch` 越界或空槽返回 `-1`。
4. **注册一个自己的系统调用**:挑一个空号(避开 0/1/24/60,也别撞测试占用的 200/201/255),内核侧写 handler、`syscall_register` 进表,用户侧在 `user/libc` 加对应封装,在 `hello.cpp` 里调它,让串口看见它的往返结果。
5. **把 GS base 区铺好、把 FPU/SSE 和栈对齐打通**:SYSCALL 入口要用 `%gs:0/%gs:8` 当 per-CPU scratch,所以 `launch_first_user` 得先分配一页当 GS base 区、`wrmsr(KERNEL_GS_BASE, ...)`;用户态 C++ 一上 SSE 就要 16 字节对齐,所以 boot.S 要开 OSFXSR、用户入口 RSP 要满足 SysV「RSP ≡ 8 mod 16」。

做完这五件,串口依次打出 `[USER] Jumping to Ring 3` / `[USER] Hello from Ring 3!` / `[SYSCALL] sys_exit: no scheduler, halting.`——Ring 3 真的「会说话」了。

## 前置条件

你得先过 Lab 022(Ring 3 跳转)、Lab 020(调度器骨架)、Lab 019(上下文切换)、Lab 018(地址空间)。关键依赖:

- **022 的 `jump_to_usermode` / `usermode_init_asm`**:已经能从 Ring 0 跳进 Ring 3、已经写了一遍 STAR/SFMASK/EFER.SCE。这一关的 `syscall_init` 会**再写一遍** STAR/SFMASK(见接口约束「STAR 两个写入点」),别被绕晕。
- **019 的 `CpuContext` / `context_switch`**:`sys_exit` 的 `Scheduler::yield()` 路径最终走到它(虽然这一关这条路径跑不到)。
- **018 的 higher-half + 用户地址空间**:`launch_first_user` 要建用户页表、映射代码页和栈页、把内核高半区 PDPT 抄进用户空间。
- **System V AMD64 ABI**:参数寄存器顺序 `rdi/rsi/rdx/rcx/r8/r9`、第 7 参进栈、函数入口 `RSP ≡ 8 mod 16`。这一关 `syscall_entry` 六参挪位的全部依据、`USER_ABI_RSP_OFFSET` 的依据,都压在这一条 ABI 上。

还要理解一条硬件事实(Intel SDM):`SYSCALL` 指令会自动做 `RCX := RIP`、`R11 := RFLAGS`、`RIP := LSTAR`、`CS := STAR[47:32] & FFFC`、`SS := STAR[47:32]+8`、`RFLAGS &= ~SFMASK`,**但它不保存 RSP**——所以入口必须自己把用户栈指针抢救下来。`SYSRETQ` 则做 `RIP := RCX`、`RFLAGS := (R11 & 3C7FD7H)|2`、`CS := STAR[63:48]+16|3`、`SS := (STAR[63:48]+8)|3`,**也不改 RSP**——所以返回前必须先把 RSP 切回用户栈。这两句「不保存/不改 RSP」是整段汇编为什么必须自己存 RSP、自己切栈的全部理由。

## 任务分解

**第一步:`syscall_init` 配三只 MSR。** MSR 地址是死的:`STAR=0xC0000081`、`LSTAR=0xC0000082`、`SFMASK=0xC0000084`。`LSTAR` 写成 `syscall_entry` 的地址;`SFMASK` 写 `0x200`(入口清掉 IF 位——进入内核那一刻中断应当关着,免得 trap frame 还没搭好就被时钟中断打断);`STAR` 的位布局要想清楚:`[47:32]` 是 SYSCALL 取的 CS 槽、`[63:48]` 是 SYSRET 取的 CS 槽,两个槽在这一关都放内核代码段 `GDT_KERNEL_CODE`。别问为什么 SYSRET 槽也放内核段、不放用户段——问就是 SDM:SYSCALL 进来时硬件用 STAR 算的是内核段,而 SYSRET 出去时硬件拿 `[63:48]` 算的是 `base+16|3`、`(base+8)|3` 这套拼上 RPL 3 的用户段。这一关选的 base 让 SYSRET 算出的用户 CS 落在 `GDT_USER_CODE`,但算出的用户 SS 并不正好等于 `GDT_USER_DATA`——这是 023 这套取值下没对齐的一处,单任务跑 `SYSCALL→sys_write→SYSRETQ` 往返时它蒙混得过去,真用起来会咬人。怎么咬、怎么修,是下一关 024 的调试现场,这一关**只按当下跑通的取值写**,不提前展开。

`syscall_init` 还要把传进来的 `kernel_rsp` 存进一个全局,供 `syscall_get_kernel_rsp()` 读取(测试要查它),并把 dispatch 表整个清成 nullptr。想清楚为什么 STAR 在这一关被 `usermode_init_asm` 和 `syscall_init` **各写一次、两次写的值相同、最终生效的是后写的 `syscall_init`**(main.cpp 的调用序是先 `usermode_init()` 再 `syscall_init()`)。

**第二步:`syscall_entry`——这一关最值也最坑的一段汇编。** 分几段写,每段都要能独立说出「为什么」:

- **`swapgs` 必须是第一条。** SYSCALL 进来时 GS 还指着用户侧、RSP 还停在用户的栈上。这一刻你拿不到内核数据结构,直接 `mov %rsp,某个内核变量` 语义上就是「在用户上下文里访问内核数据」,乱套。`swapgs` 把 GS.base 和 `KERNEL_GS_BASE`(0xC0000102)一互换,GS 立刻指向内核 per-CPU 区,之后 `%gs:offset` 就稳稳落在内核私有的 scratch 页上。
- **存用户 RSP、换内核栈**:用 `%gs:8` 暂存用户 RSP、用 `%gs:0` 载入内核 RSP。这两个槽来自 `launch_first_user` 里分配的那一页 GS base 区(第 0 槽存内核 RSP0、第 1 槽留给用户 RSP)。**SYSCALL 不保存 RSP**,所以这两个槽必须由你自己建。
- **搭 trap frame**:在内核栈上按固定顺序压栈。这一关**不给完整 push 序列、也不给每个槽的字节偏移数字**——那一套偏移正是你要自己算出来的核心结论。给你的只有结构(从栈顶往下那一列的次序):`user_rsp / user_rip(=RCX) / user_rflags(=R11) / syscall_nr(=RAX) / rdi / rsi / rdx / r10 / r8 / r9 / rbx / rbp`——12 个 8 字节槽,每槽 8 字节。注意 push 的**先后顺序和最终偏移是反的**:最后压的那个落在栈顶(最低地址、相对 RSP 偏移 0),最先压的那个落在最底(最高地址)。所以让 `user_rsp` 待在偏移 0,它就得是最后压的那一个;`rbp` 待在最底,它就得是最先压的。其余十个槽各自的偏移,你自己照「12 槽每槽 8 字节、后压的在低地址」推一遍,在注释里钉死,后面所有 `mov N(%rsp),%reg` 都靠它。这里有个和普通 C 调用约定的关键分歧:第 4 个参数存的是 `%r10` 而不是 `%rcx`——因为 SYSCALL 把 RCX 抢去存返回地址了,用户态约定把第 4 参挪到 R10。frame 里如实记成 R10;不过它进 dispatch 时并不会回到 rcx——dispatch 的首参 nr 占了 rdi,SysV 寄存器顺移一位,于是 arg3(RDX 槽)才进 rcx、arg4(R10 槽)进 r8。arg4→r8 这一步下一节细算。
- **第 7 个参数挪到栈上**:这是 trap frame 之后最容易翻车的一步。SysV C ABI 前 6 参走 `rdi/rsi/rdx/rcx/r8/r9`,可我们的 6 个参数在 frame 里的位置和 SysV 要求的寄存器对不上(frame 里 arg4 在 R10、arg5 在 R8、arg6 在 R9,而 SysV 要 arg4 走 rcx、arg5 走 r8、arg6 走 r9)。所以先得把 arg6(frame 里那个代表原 R9 的槽)`push` 到栈上当第 7 个 C 参数——**这一 push 让整个 frame 相对 RSP 往高地址挪了一格,于是你取 nr、arg1..arg5 时,偏移全部要 +8**。具体每个槽 push 之后变成多少字节偏移,这一关不给,你自己按上一步钉死的基线偏移加 8 算出来填进 `mov N(%rsp),%reg`;nr、arg1..arg5 从 +8 后的偏移取进 rdi/rsi/rdx/rcx/r8/r9——注意因为 nr 占了 rdi、参数顺移一位,arg3 才进 rcx、arg4 进 r8、arg5 进 r9。具体说,把 arg3 从 frame 的 RDX 槽读进 `rcx`、arg4 从 R10 槽读进 `r8`、arg5 从 R8 槽读进 `r9`、arg6... 等等,arg6 呢?arg6(原 R9)是开头那条 `push` 单独上栈当第 7 个 C 参的,它走栈、不进寄存器。`call syscall_dispatch` 之后 `add $8,%rsp` 把这个临时第 7 参销毁掉。想清楚**为什么必须先 push 再取参、为什么 push 之后所有偏移都变**——这条没想通,参数顺序全乱。
- **返回值绕道 `%rbx`**:dispatch 的返回值在 `%rax`。可接下来你要从 frame 恢复 `%rcx`(SYSRETQ 从 rcx 取 RIP)、恢复 `%r11`(从 r11 取 RFLAGS)、`add $96` 销毁整个 12 槽 frame、`mov %gs:8,%rsp` 切回用户栈——这一串中间任何一条都可能盖掉 rax。SYSRETQ 不碰 rbx,所以把返回值先 `mov %rax,%rbx`,等切回用户栈之后再用 `mov %rbx,%rax` 还回去。注意两个顺序:**恢复 `%rcx/%r11` 必须在销毁 frame 之前**(frame 销毁后那两个槽就没了);**切回用户栈(`mov %gs:8,%rsp`)必须在 SYSRETQ 之前**(SYSRETQ 不改 RSP,你给它什么 RSP,它就在什么 RSP 上回用户态)。
- **出口 `swapgs` + `sysretq`**:再 `swapgs` 把 GS 换回用户侧,然后 `sysretq` 回 Ring 3。`swapgs` 入口一次、出口一次,必须配对。

**第三步:`syscall_register` + `syscall_dispatch` + 三个 handler。** dispatch 表是个 `SyscallFn[256]`,`SyscallFn = int64_t(*)(uint64_t×6)`——所有 handler 签名一致,dispatch 才能用一张表统一管。`syscall_register(nr, fn)` 把 fn 填进 `table[nr]`,越界直接返回不填。`syscall_dispatch` 是个 `extern "C"` 的 C 链接函数,被汇编 `call`:越界(`nr >= 256`)或空槽(`table[nr]==nullptr`)返回 `-1`,空槽还 `kprintf` 一句「unhandled syscall」。三个 handler 签名都是 `int64_t(uint64_t a1..a6)`:`sys_write(fd,buf_virt,count,...)` 有两道校验——`buf_virt >= 0x800000000000` 返回 -1(拒绝内核地址,这是这一关**唯一**的地址校验,不是真正的 copy_from_user,它不查页映射、不处理缺页)、`fd != 1` 返回 -1,过了就逐字节 `kprintf("%c")` 把字符往串口和 Console 送,返回 count;`sys_exit(code,...)` 把当前 task 标 Dead,然后**双分支**:`Scheduler::is_initialized() ? yield() : cli;hlt` 死循环;`sys_yield(...)` 直接 `Scheduler::yield()`。想清楚为什么 `sys_exit` 这一路在本 tag 必然走 halt 分支(见「常见故障」)。

**第四步:注册一个你自己的 syscall。** 挑一个空号(避开 0/1/24/60,也别撞到测试占用的 200/201/255),比如给 `SyscallNr` 加一个 `SYS_getpid = 39`。内核侧写 `int64_t sys_getpid(uint64_t×6)` 返回一个固定值(比如 `42`),`syscall_register` 进表。用户侧在 `user/libc/syscall.cpp` 加 `int64_t sys_getpid(void){ return _syscall1(SYS_getpid, 0); }`,在 `user/libc/syscall.h` 加声明。在 `hello.cpp` 里 `sys_write` 之前先调它,把返回值想办法让串口看见——比如用它当循环次数,或者 `sys_write` 一段长度等于返回值的串。这一步逼你完整走一遍「内核加号、填表、用户加封装、用户调用、串口验收」的闭环。这一步里有个让你「不能偷懒」的关卡:用户侧 `_syscall3` 这种内联汇编封装的 clobber 列里**必须**写上 `rcx`、`r11`、`memory`——少写一个,编译器就可能假设这俩寄存器跨调用不变,优化出灾难。

**第五步:GS base 区 + FPU/SSE + 栈对齐。** `launch_first_user` 里:分配一页物理页当 GS base 区,`gs_virt[0] = kernel_rsp0`、`gs_virt[1] = 0`,`wrmsr(KERNEL_GS_BASE, gs_virt)`——这一步不做好,`syscall_entry` 的 `%gs:0` 读到的是垃圾,换栈就炸。boot.S 要在 `cli` + `mov $imm,%rsp` 之后开 FPU:CR4 置 `OSFXSR`(`1<<9`)+`OSXMMEXCPT`(`1<<10`)、CR0 清 `EM`(`1<<2`)、置 `MP`(`1<<1`)、`clts`。`Task` 加 `alignas(16) uint8_t fpu_state[512]`,`TaskBuilder::build()` 里 `fninit + fxsave` 给每个任务初始化一份干净状态,`schedule/exit_current/run_first` 三处在 `context_switch` 前后配 `fxsave`(存当前)/`fxrstor`(恢复下一个)。用户入口 RSP 要满足 SysV 对齐:`USER_STACK_TOP - USER_ABI_RSP_OFFSET`(offset=8)交给 `jump_to_usermode`,并配一道编译期断言 `static_assert((USER_STACK_TOP - 8) % 16 == 8)` 把这条 ABI 契约焊死。

## 接口约束

你要实现出来的东西,对外长这样(职责与签名,不给实现):

- `enum class SyscallNr : uint64_t { SYS_read=0, SYS_write=1, SYS_yield=24, SYS_exit=60 };` + `constexpr uint64_t SYSCALL_TABLE_SIZE = 256;`(刻意对齐 Linux x86-64。)
- `using SyscallFn = int64_t(*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);`
- `void syscall_init(uint64_t kernel_rsp);`——写 STAR/LSTAR/SFMASK,清空 dispatch 表,存 `kernel_rsp`。必须在 `usermode_init()` 之后调。
- `void syscall_register(SyscallNr nr, SyscallFn handler);`——越界不填。
- `uint64_t syscall_get_kernel_rsp();`——读回 init 时存的 RSP。
- `extern "C" void syscall_entry();`——LSTAR 的目标,绝不从 C 直接调。
- `extern "C" int64_t syscall_dispatch(uint64_t nr, uint64_t a1..a6);`——被汇编 `call`,越界/空槽返回 -1。
- `int64_t sys_write/sys_exit/sys_yield(uint64_t×6);`——三个 handler。
- 用户侧:`int64_t sys_read/sys_write(int, void*, size_t);` / `void sys_exit(int);` / `void sys_yield(void);`——`syscall` 内联汇编封装,clobber `rcx/r11/memory`。

关键约束(违反就翻车):

- **trap frame 的 12 槽顺序与偏移,必须和你汇编里 push 的先后逐字节对上**。push 顺序与最终偏移相反(后压的在栈顶、相对 RSP 偏移 0),自己把每个槽的字节偏移画一遍并在注释里钉死。这套偏移是「第 7 参压栈后 +8」之前的基线;压了第 7 参之后,取 nr、arg1..arg5 的偏移全要 +8。
- **第 7 个参数必须先 `push` 再取 nr、arg1..arg5**,且 push 之后所有 frame 偏移 +8。arg3(RDX 槽)读进 rcx、arg4(R10 槽)读进 r8——因为 dispatch 的首参 nr 占了 rdi、参数顺移一位(arg4 不会回到 rcx,rcx 这一格让给了 arg3);arg6(原 R9)走开头那条 `push` 当第 7 参,不进寄存器。
- **返回值必须经 `%rbx` 中转**:SYSRETQ 从 rax 取返回值、从 rcx 取 RIP、从 r11 取 RFLAGS,但恢复 rcx/r11、销毁 frame、切栈这一串会盖掉 rax;SYSRETQ 不碰 rbx,所以先存 rbx、切栈后再 `mov %rbx,%rax`。
- **恢复 `%rcx/%r11` 必须在 `add $96` 销毁 frame 之前**;切回用户栈(`mov %gs:8,%rsp`)必须在 `sysretq` 之前(SYSRETQ 不改 RSP)。
- **`swapgs` 两次**:入口一次(换内核 GS)、出口一次(换回用户 GS)。漏出口那次,下次进用户态 GS 就指错地方了。
- **STAR 被写两次**:`usermode_init_asm`(汇编,先调)和 `syscall_init`(C++,后调)各写一遍 STAR,在这一关两边写的值相同(都把内核代码段塞进 SYSCALL 槽 `[47:32]` 和 SYSRET 槽 `[63:48]`,得到 `star_val = (GDT_KERNEL_CODE<<32)|(GDT_KERNEL_CODE<<48)`)。**最终生效的是后写的 `syscall_init` 那次**。别把这两次当成「冗余 bug」——它们目标一致,只是历史地分在两处。调试时如果你改了 `syscall_init` 里的 STAR、却读回是另一个值,先想想要不是 `usermode_init_asm` 那一份还没盖掉。
- **`sys_write` 的地址校验只有一道**:拒绝 `buf_virt >= 0x800000000000`(canonical address 分水岭,高于它的就是内核半区,不是真正的 copy_from_user,不查页映射);`fd` 只认 1。别把它当成 Linux 的 VFS——没有 fd 表、没有缓冲区、没有真正的「写文件」。
- **`sys_exit` 本 tag 走 halt 分支**:`launch_first_user()` 之前没启调度器,`is_initialized()` 为假 → `cli;hlt` 死循环;那条 `yield()` 分支是「为 024 留的、本 tag 跑不到」的代码。别在本 tag 声称 sys_exit 会切换到下一个用户进程。
- **MSR 地址与 STAR 位段给死**:`STAR=0xC0000081`、`LSTAR=0xC0000082`、`SFMASK=0xC0000084`、`KERNEL_GS_BASE=0xC0000102`、`EFER=0xC0000080`;STAR 两槽都填 `GDT_KERNEL_CODE`,SYSCALL 时 `CS = STAR[47:32] & FFFC`、`SS = STAR[47:32]+8`,SYSRET 时 `CS = (STAR[63:48]+16)|3`、`SS = (STAR[63:48]+8)|3`。

汇编里 push 的具体指令次序、第 7 参 push 后 `mov N(%rsp),%reg` 的具体偏移数字、`call`/`add $8`/`mov %rax,%rbx` 的完整序列——这些**这一关不提供**,你自己写,但写下来就要和上面的 frame 布局对齐,并在注释里把每个偏移标清楚。

## 验证步骤

**Host 侧单测**:把 `SyscallNr` 常量、`SYSCALL_TABLE_SIZE`、dispatch 表(register/覆写/越界 -1/空槽 -1)、`sys_write` 的 fd 与 `USER_ADDR_MAX` 校验、`sys_exit` 的 state→Dead、STAR MSR 值计算、`SyscallFn` 签名,在 host 侧重写一份(mock,不链内核代码、`CINUX_HOST_TEST` 门控):

```bash
ctest --test-dir build -R syscall --output-on-failure
```

**QEMU 机内测**:真 MSR(`rdmsr` 读回 LSTAR≠0、STAR 的 `[47:32]` 与 `[63:48]` 都是 `GDT_KERNEL_CODE`、SFMASK 的 wrmsr 不 #GP)、真 dispatch(自定义 handler slot 200/201、全 6 参传递、未注册 -1、越界 256/1024 -1、slot 255 最大合法)、`syscall_get_kernel_rsp` 非零、`sys_write` 直调(fd=1 返回 count、fd≠1 返回 -1、`buf_virt≥0x800000000000` 返回 -1):

```bash
cmake --build build --target run-big-kernel-test
```

机内会打 test section `Syscall Tests (023)`,全过、末尾 `ALL TESTS PASSED`。

**生产 demo**:直接跑大内核的 QEMU 运行目标。预期串口依次出现:

```text
[BIG] ===== Milestone 023: Syscall from Ring 3 =====
[USER] Setting up first user-mode program...
[USER] Jumping to Ring 3: entry=0x0000000000400000 stack=0x00000007FFFFF000
[USER] Hello from Ring 3!
[SYSCALL] sys_exit: no scheduler, halting.
```

盯最后那句 `sys_exit: no scheduler, halting.`——它出现就证明你走的是 halt 分支(本 tag 该有的行为),不是 yield。为什么串口里看不到 `sys_exit` 那句带 `tid`/`name` 的日志?因为本 tag 在 `launch_first_user` 之前没调 `Scheduler::init()`,`Scheduler::current()` 返回 nullptr,而 `sys_exit.cpp` 里那条 `kprintf("... from tid=%u '%s' ...")` 整个套在 `if (task != nullptr)` 里——task 为空时它被跳过,直接落到 else 打出 halt 那行。这正是不走 yield 分支、落到 halt 的判据。你新加的自定义 syscall,也要在 `hello.cpp` 跑完时在串口留下它的往返痕迹(返回值被你设计成可见的输出)。

## 常见故障

- **用户态 `movaps [rsp],xmm0` 触发 #GP,RIP 落在 `hello.cpp` 入口附近几条指令内**:根因可能叠了两层,别一上来就只改一处。第一层:GCC 把 `const char msg[]="..."` 的初始化优化成 SSE `movdqa/movaps`,要求目标 16 字节对齐——先确认 boot.S 真的开了 CR4.OSFXSR/OSXMMEXCPT、清了 CR0.EM、`clts` 清了 TS。第二层(开了 FPU 还 GP):栈不满足 SysV「入口 RSP ≡ 8 mod 16」——`USER_STACK_TOP` 本身是 0 mod 16,得 `USER_STACK_TOP - 8` 交给 `jump_to_usermode`。修复:加 `USER_ABI_RSP_OFFSET=8` + `static_assert`。**教训:一个 #GP 可能叠了两层病因(FPU 未启 + 对齐),定位时分开验证——先单独验 FPU,再单独验对齐,别两把一起拧。**

- **boot.S 加了 FPU 初始化后,`run-big-kernel-test` 输出 `Loaded ELF is not a real kernel, exiting`,169 个大内核测试全跳过**:mini kernel 用大内核入口的前 3 字节验真——模式是 `FA 48 BC/C7`(即 `cli` + `mov $imm,%rsp`)。你把 CR4/CR0/clts 那一坨 FPU 初始化插到这两条**前面**,字节序列变成了 `FA 0F 20`,判否。修复:把 FPU init 挪到栈设置(`movq $__kernel_stack_top,%rsp`)**之后**,保住前两条指令的 `FA 48 BC/C7` 字节模式。**教训:改启动汇编要盯死「被外部工具当签名校验」的字节序列——mini kernel 拿前 3 字节认你是不是真内核。**

- **STAR 写了但 `rdmsr` 读回值不对 / SYSCALL 一执行就 #GP / 跳到错误段**:大概率是 STAR 两个写入点搞混了。`usermode_init_asm`(汇编,先调)和 `syscall_init`(C++,后调)各写一遍 STAR,两边值必须一致(都把 `GDT_KERNEL_CODE` 放进 SYSCALL 槽和 SYSRET 槽)。确认 `syscall_init` 在 `usermode_init` **之后**调(它是最终生效的那次);确认 EFER.SCE 在 `usermode_init_asm` 里置上了(SYSCALL 的触发条件之一,没置会 `#UD`)。

- **dispatch 拿到的 arg4 是错的 / arg 顺序整体错位**:第 7 参没先 push,或者 push 之后忘了「所有 frame 偏移 +8」。记住:取 nr、arg1..arg5 的那条 `mov` 在 push 第 7 参之后,基线偏移整体 +8(因为你的 frame 顶还有 user_rsp/user_rip/user_rflags/syscall_nr 四个槽,加上 push 的一个)。把 push 后、call 前的栈图画一遍,每个 `mov N(%rsp),%reg` 的 N 都对着画好的偏移填。还有一处易读漏:r9 拿的不是 arg6,而是 frame 里的 R8(原 arg5);真正的 arg6(原 R9)是开头那条 `push` 单独上栈当第 7 参的。

- **`sysretq` 前没切回用户栈,返回后栈是内核栈、写一下就崩**:忘了 `mov %gs:8,%rsp`。SYSRETQ 不改 RSP,你不动它,它就还是内核栈指针,用户态一返回就踩在自己的栈帧之外。

- **返回值永远是 0 或脏值,用户拿到的不是 handler 的返回值**:返回值没经 rbx 中转,被恢复 rcx/r11 或 `add $96` 之后的某条指令盖了。记住顺序:`mov %rax,%rbx` 存 → 从 frame 恢复 rcx/r11 → `add $96` 销毁 frame → `mov %gs:8,%rsp` 切回用户栈 → `mov %rbx,%rax` 还。

- **`swapgs` 只在入口写了一次,出口忘了**:第二次进用户态(或下一次 syscall)时 GS 还指着内核侧,`%gs:0/%gs:8` 读出内核数据当用户栈指针,换栈即崩。`swapgs` 入口出口必须成对。

- **把 `sys_exit` 的 yield 分支当成「已实现」**:本 tag 这条路径跑不到(调度器此路径未启),你若手贱在 `launch_first_user` 前启了调度器想让它走 yield,反而可能把单任务 demo 搞崩(没有下一个真任务可切)。这一关就让它 halt 收尾,yield 留给 024。

## 通过标准

1. **MSR 配置正确**:`syscall_init` 写 LSTAR=`syscall_entry`、`STAR` 两槽都填 `GDT_KERNEL_CODE`、`SFMASK=0x200`;`rdmsr` 读回 LSTAR 非零、`STAR[47:32]` 与 `STAR[63:48]` 都等于 `GDT_KERNEL_CODE`;SFMASK 的 wrmsr 不 #GP。说清 STAR 被 `usermode_init_asm` 与 `syscall_init` 各写一次、值相同、后者最终生效。
2. **`syscall_entry` 汇编行为正确**:入口 `swapgs` → `%gs:8/%gs:0` 存/换栈 → 12 槽 trap frame(顺序/偏移自洽,user_rsp 在顶、rbp 在底,各槽具体字节偏移你在注释里自己算清并钉死)→ 第 7 参 `push` 后取参偏移 +8、arg3 进 rcx 而 arg4(R10 槽)进 r8 → `call syscall_dispatch` → 返回值经 rbx 中转 → 恢复 rcx/r11 → `add $96` → 切回用户栈 → 还 rax → 出口 `swapgs` → `sysretq`。
3. **dispatch 表正确**:`SYSCALL_TABLE_SIZE=256`、register 填表、dispatch 越界/空槽返回 -1;三个 handler 签名一致(`SyscallFn`)、`sys_write` 两道校验(地址上界 + fd==1)、`sys_exit` 双分支(本 tag 走 halt)。
4. **用户态编译基建跑通**:`user/CMakeLists.txt` 三步(编 ELF → `objcopy -O binary` 抽 flat binary → `ld -r -b binary` 包成 `_binary_hello_bin_start/end` 符号);`user/libc/syscall.cpp` 的 `syscall` 内联汇编 clobber rcx/r11/memory;`hello.cpp` 的 `_start` 调 `sys_write(1, msg, 26)` + `sys_exit(0)`,入口是 `_start` 不是 `main`(因为 `-nostdlib`,没有 crt 替你调 main)。
5. **FPU/SSE + 栈对齐打通**:boot.S 开 OSFXSR/OSXMMEXCPT、清 EM、`clts`;`Task.fpu_state[512]` alignas 16、`build()` 里 fninit+fxsave、调度器三处 fxsave/fxrstor;`USER_ABI_RSP_OFFSET=8` + static_assert。
6. **自定义 syscall 往返成功**:你加的号被内核侧 dispatch 命中你写的 handler、用户侧封装能调到它、串口见到它返回值的效果。
7. **测试全绿**:`ctest -R syscall` 过;QEMU 机内 `Syscall Tests (023)` 全过、末尾 `ALL TESTS PASSED`;生产 demo 串口见 `[USER] Hello from Ring 3!` + `[SYSCALL] sys_exit: no scheduler, halting.`。
8. **三个调试坑能复述症状与根因**:用户态 movaps #GP(FPU 未启 + 栈对齐两层)、boot.S 字节序列被 mini kernel 判否、sys_exit 走 halt 而非 yield(设计性解耦,非 bug)。

做到这八条,Ring 3 就第一次「会说话」了。但 hello 只会说一句就 exit——没有 read、没有 shell、`sys_exit` 走 halt 不能常驻。下一关(024)会接上 `sys_read`(键盘输入)、写一个真正的 shell(echo/help/clear)、在 `launch_first_user` 前启调度器让 `sys_exit` 走 yield、shell 常驻。顺带它会撞上 SYSRET 的 SS RPL 和 `syscall_entry` 里 rbx clobber 两个真坑——那是下一关的调试现场,这一关别提前剧透。
