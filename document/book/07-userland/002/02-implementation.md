---
title: 02 · 代码路线:MSR、syscall_entry、dispatch 表与用户态编译
---

# 代码路线:MSR、syscall_entry、dispatch 表与用户态编译

## 三只 MSR:SYSCALL 怎么知道往哪跳、用什么段、清哪些 flag

[syscall.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/syscall.cpp) 的 `syscall_init` 干的事,翻译成人话就是「给硬件留三个地址」。看核心几行:

```cpp
constexpr uint32_t MSR_STAR   = 0xC0000081;
constexpr uint32_t MSR_LSTAR  = 0xC0000082;
constexpr uint32_t MSR_SFMASK = 0xC0000084;

uint64_t star_val = (static_cast<uint64_t>(GDT_KERNEL_CODE) << 32)
                  | (static_cast<uint64_t>(GDT_KERNEL_CODE) << 48);
write_msr(MSR_STAR, star_val);                       // 两槽都填 0x08
write_msr(MSR_LSTAR, reinterpret_cast<uint64_t>(syscall_entry));
write_msr(MSR_SFMASK, 0x200);                        // bit9 = IF
```

`STAR` 把同一个值 `0x08` 同时塞进 `[47:32]`(SYSCALL 取)和 `[63:48]`(SYSRET 取)两个槽。为什么都填 0x08?因为 SYSCALL 进来要用它当**内核**代码段(`CS = STAR[47:32] & FFFC = 0x08`),而 SYSRET 出去时,硬件拿 `[63:48]` 算**用户**段:`CS = (STAR[63:48] + 16) | RPL = (0x08 + 16) | 3 = 0x1B`、`SS = (STAR[63:48] + 8) | RPL = (0x08 + 8) | 3 = 0x13`。用户 CS 这半句算下来正好等于 `GDT_USER_CODE`(0x1B),没问题;可用户 SS 算出来是 0x13,并不等于 `GDT_USER_DATA`(0x23)。这是 023 这套 STAR 取值下没对齐的一处——单任务跑 `SYSCALL→sys_write→SYSRETQ` 往返时,因为同一段寄存器一直是这个值、没人另设 SS,它能蒙混过去;可一旦多任务或中断往返把 SS 换成别的值,这 0x13 就会咬人。这个坑怎么定位、怎么修,是下一站(024)的调试现场,这里只点破它没对齐,不展开。

`LSTAR` 直接指向 `syscall_entry` 的地址,这就是 SYSCALL 的落点。`SFMASK=0x200` 让硬件在入口执行 `RFLAGS ← RFLAGS AND NOT 0x200`,即把 IF 清掉——syscall 进来那一刻中断是关的,免得 trap frame 还没建好就被时钟中断打断。

这里有个容易踩混的点,得专门说清:**STAR 在 023 被写了两次**。`usermode_init()`(在汇编 `usermode_init_asm` 里,先于 `syscall_init` 调用)也写了一遍 STAR——它用 `movq $0x08,%rdx; shlq $16,%rdx; orq $0x08,%rdx` 把同样的 `0x08/0x08` 拼进去。两边写法不同(汇编靠移位、C++ 靠字面量),但**值完全一致**。`main.cpp` 里的调用序是先 `usermode_init()` 再 `syscall_init()`,所以最终生效的是后者那一次——但因为两者目标一致,谁最后写都一样。把这件事想明白,就不会在调试时困惑「我明明在 syscall.cpp 里改了 STAR,为什么读回来是另一个值」。

## syscall_entry:swapgs、换栈、建 trap frame

[syscall.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/syscall.S) 的开头,是整段最容易写错的部分:

```asm
syscall_entry:
    swapgs                              # GS.base ↔ KERNEL_GS_BASE
    movq %rsp, %gs:8                    # 把用户 RSP 藏进 per-CPU scratch
    movq %gs:0, %rsp                    # 载入内核栈指针
```

为什么是 `swapgs` 而不是先碰栈?因为这一刻 CPU 还在用户的 `RSP` 上、`GS` 还指着用户侧。要是先 `mov %rsp,...` 把用户 RSP 存到某个「内核变量」里,语义上就是「在用户上下文里访问内核数据」,乱套。`swapgs` 先把 GS 换成内核的 per-CPU 基址,之后的 `%gs:0`、`%gs:8` 就稳稳落在内核私有的 scratch 页上。那个 scratch 页是在 `launch_first_user` 里现造的:分配一页物理页,第一格 `gs_virt[0] = kernel_rsp0` 存当前内核栈顶,第二格清 0,然后 `wrmsr(MSR_KERNEL_GS_BASE 0xC0000102, gs_virt)` 把它登记成 KERNEL_GS_BASE。没有这一步,`swapgs` 之后 `%gs:0` 读出来的就是垃圾,栈一换就炸。

换好栈,开始建 frame。注意 push 顺序和最终偏移是**反过来**的——先 push 的落到栈底(高地址):

```asm
    pushq %rbp        # 落到 +88 (callee-saved, 先存)
    pushq %rbx        # +80
    pushq %r9         # +72  arg6
    pushq %r8         # +64  arg5
    pushq %r10        # +56  arg4  ← 注意是 R10
    pushq %rdx        # +48  arg3
    pushq %rsi        # +40  arg2
    pushq %rdi        # +32  arg1
    pushq %rax        # +24  syscall 号
    pushq %r11        # +16  user RFLAGS
    pushq %rcx        # +8   user RIP
    movq %gs:8, %rax  # 取回刚才存的用户 RSP
    pushq %rax        # +0   user RSP (最后 push, 在顶)
```

第 4 个参数存的是 `%r10`,不是 `%rcx`——这是 SYSCALL 调用约定和普通 SysV C ABI 的关键分歧。SysV 说 C 函数第 4 个参数走 `%rcx`;可 SYSCALL 硬件把 `RCX` 抢去存返回地址了,于是用户态约定把第 4 参挪到 `R10`。frame 里如实记成 R10,等会儿 dispatch 时再挪回 rcx。

## 从 trap frame 到 C 函数:第 7 个参数的栈上挪位

`syscall_dispatch` 是个普通的 SysV C 函数,签名是 7 个参数:`(nr, a1..a6)`。前 6 个走寄存器(`rdi/rsi/rdx/rcx/r8/r9`),第 7 个得上栈。可我们的 6 个参数在 frame 里的位置,和 SysV 要求的寄存器对不上——frame 里 arg4 在 R10、arg5 在 R8、arg6 在 R9,而 SysV 要 arg4 走 rcx、arg5 走 r8、arg6 走 r9。所以得重排,这一段是整条链里最绕的:

```asm
    movq 72(%rsp), %rax      # 取 frame+72 = arg6 (原 R9)
    pushq %rax               # 先把它压栈, 当第 7 个 C 参 → 此后所有 frame 偏移 +8!

    movq 32(%rsp), %rdi      # +32: 注意! 原来 nr 在 +24, push 了一格后变 +32
    movq 40(%rsp), %rsi      # arg1 (原 RDI)
    movq 48(%rsp), %rdx      # arg2 (原 RSI)
    movq 56(%rsp), %rcx      # arg3 ← frame 里的 RDX (原 arg3 原样)
    movq 64(%rsp), %r8       # arg4 ← frame 里的 R10 (原 arg4)
    movq 72(%rsp), %r9       # arg5 ← frame 里的 R8  (原 arg5)

    call syscall_dispatch
    addq $8, %rsp            # call 返回, 把第 7 参从栈上抹掉
```

为什么 `push %rax` 之后,后面取参的偏移全都 +8?因为 push 让 RSP 减了 8,frame 整体相对 RSP 往高挪了一格。原本 nr 在 `+24`,push 后就跑到 `+32` 了——这不是笔误,是栈生长方向的必然。把这点想通,就不会写出「nr 取错位、dispatch 到了乱七八槽的号」的 bug。

还有一处容易读漏:上面那 6 条 `mov` 只把 nr、arg1..arg5 喂进了寄存器(`rdi/rsi/rdx/rcx/r8/r9`),第 6 个 C 参(r9)拿的其实是 frame 里的 R8(原 arg5),而不是 arg6。真正的 arg6(frame 里的 R9)是靠开头那条 `mov 72(%rsp),%rax; push %rax` 单独上栈当第 7 个 C 参的——它在 push 之前、frame 还没 +8 时就读走了 `+72` 的旧值(=arg6 R9),所以不走这 6 条 mov。

还有一处要盯死:`nr` 取的是 `frame+32`(即 `+24` 的 syscall 号),但 SysV 要求 C 的**第 1 个参数**走 `rdi`。所以这里 `mov 32(%rsp),%rdi` 是把「syscall 号」当第 1 参传进 `syscall_dispatch(nr, a1..a6)`。frame 里的 arg1(RDI)反而成了 C 的第 2 参(走 `rsi`)。编号错位一位,是「syscall 号要占住第 1 参位」的代价。

## 返回路径:为什么返回值要绕道 rbx

`syscall_dispatch` 返回后,返回值在 `%rax`。按理直接 `sysretq` 就完事——SYSRETQ 本来就从 `rax` 取返回值。可问题是:在「拿到返回值」和「执行 sysretq」之间,还有一连串指令要动寄存器——要恢复 user RIP(进 rcx)、user RFLAGS(进 r11)、要销毁 frame(`add $96`)、要切回用户栈(`mov %gs:8,%rsp`)。这些指令但凡有一条顺手用了 rax,返回值就没了。

解法是把返回值先寄存到一个 SYSRETQ 绝不会碰的寄存器里。callee-saved 的 `%rbx` 是天然人选——SYSRETQ 的语义(`RIP←RCX`、`RFLAGS←R11`、CS/SS 从 STAR 算)压根不提 rbx:

```asm
    movq %rax, %rbx          # 返回值先寄存到 rbx (SYSRETQ 不碰它)

    movq 0(%rsp), %rax       # 取 user RSP
    movq %rax, %gs:8         # 记回 scratch (待会儿切栈要用)
    movq 8(%rsp), %rcx       # 恢复 user RIP (SYSRETQ 从 rcx 取)
    movq 16(%rsp), %r11      # 恢复 user RFLAGS (SYSRETQ 从 r11 取)

    addq $96, %rsp           # 销毁整个 12 槽 frame
    movq %gs:8, %rsp         # 切回用户栈
    movq %rbx, %rax          # 把返回值从 rbx 还回 rax

    swapgs                   # GS 换回用户侧 (入口换过一次, 出口必须换回来)
    sysretq
```

`swapgs` 入口一次、出口一次,必须配对——入口把 GS 从用户侧换到内核侧,出口得再换回去,否则下次进用户态 GS 就指错地方了。「切回用户栈」(`mov %gs:8,%rsp`)必须在 `sysretq` **之前**:因为 SYSRETQ 不改 RFLAGS 里的 TF/IF 之外的栈语义、更不动 RSP,你给它什么 RSP,它就在什么 RSP 上回用户态。要是在切栈之前就 `sysretq`,用户态一返回就踩在自己的栈帧之外,立刻炸。

这一版「返回值绕道 rbx」是 023 的实际写法,它在当前这套 GDT 布局下能跑通。它是不是「最终最优」,这一章不下结论——那是后续要打磨的地方。

## dispatch 表 + 三个 handler

[syscall_nums.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/syscall_nums.hpp) 的枚举刻意对齐 Linux x86-64 的号(read=0、write=1、yield=24、exit=60)。这么做的好处不是「能跑 Linux 程序」(差得远),而是让以后真要移植用户程序时,号能对上、少改一处。`SYSCALL_TABLE_SIZE=256` 给了足够的槽,`SyscallFn` 统一成「6 个 uint64 进、一个 int64 出」的函数指针——所有 handler 签名一致,dispatch 才能用一张表统一管。

`syscall_dispatch` 本身极简:越界(`nr >= 256`)返回 `-1`,空槽(`table[nr]==nullptr`)打一行 `[SYSCALL] unhandled` 也返回 `-1`,否则 `table[nr](a1..a6)`。两个 handler 值得单独看。

[sys_write.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/sys_write.cpp) 是「朴素到寒酸」的典范:

```cpp
constexpr uint64_t USER_ADDR_MAX = 0x800000000000ULL;

int64_t sys_write(uint64_t fd, uint64_t buf_virt, uint64_t count,
                  uint64_t, uint64_t, uint64_t) {
    if (buf_virt >= USER_ADDR_MAX) return -1;   // 拒绝内核地址
    if (fd != 1) return -1;                      // 只认 stdout
    const auto* buf = reinterpret_cast<const char*>(buf_virt);
    for (uint64_t i = 0; i < count; i++) kprintf("%c", buf[i]);
    return static_cast<int64_t>(count);
}
```

两道校验:地址上界 `0x800000000000`(canonical address 的分水岭,高于它的就是内核半区,用户不该传)、`fd==1`。它没有 VFS、没有 fd 表、没有缓冲区、没有真正的「写文件」——就是逐字节 `kprintf("%c")` 把字符往串口和 Console 送。这距离 Linux 的 `write(2)` 差着十万八千里,但对 023 的目标(证明通道通)够用了。注意那道地址校验只是「上界」,不是真正的 `copy_from_user`:它不检查页是否映射、不处理缺页。用户传个没映射的地址进来,`kprintf` 读到那字节时会缺页——那是 023 留着的口子。

[sys_exit.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/sys_exit.cpp) 有一处必须说清的「设计性分歧」:

```cpp
auto* task = Scheduler::current();
if (task != nullptr) task->state = TaskState::Dead;

if (Scheduler::is_initialized()) {
    Scheduler::yield();                 // 调度器在: 让出 CPU
} else {
    kprintf("[SYSCALL] sys_exit: no scheduler, halting.\n");
    while (1) asm volatile("cli; hlt"); // 调度器没启: 死循环停机
}
```

`launch_first_user` 之前,`main.cpp` **没有**调 `Scheduler::init()`——020 写好的调度器在这条路径上压根没启动。所以 023 跑生产 demo 时,`sys_exit` 走的是 `else` 分支:`cli;hlt` 死循环,串口收尾是那句 `[SYSCALL] sys_exit: no scheduler, halting.`。这不是 bug,是刻意的解耦:`yield` 那条分支是「为 024 留的、本 tag 跑不到」的代码。这样写的好处是 syscall 模块在「有调度器」「无调度器」两种环境都能干净收场,不把里程碑之间的耦合硬拧在一起。

## 用户态编译基建:从 hello.cpp 到嵌入内核的镜像

022 的用户程序是 4 字节机器码,023 把它换成了一个真 C++ 程序。这套基建是 [CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/CMakeLists.txt) 三步搭出来的:

第一步,把 [hello.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/programs/hello.cpp) 编成 ELF——用 `-mcmodel=small`(用户态在低 2GB)、`-ffreestanding -nostdlib -static -fno-pie`,链接脚本 [linker.ld](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/linker.ld) 把 `USER_VMA` 定在 `0x400000`、`.text.start` 段放最前:

```cpp
extern "C" void _start() {
    const char msg[] = "[USER] Hello from Ring 3!\n";
    sys_write(1, msg, 26);
    sys_exit(0);
}
```

入口为什么是 `_start` 而不是 `main`?因为我们 `-nostdlib`,没有 libc 的 crt 帮你调 `main`——`_start` 是 ELF 的 `ENTRY`,内核跳进来就直接落在这儿。`sys_exit(0)` 之后那行 `__builtin_unreachable()`(在 [syscall.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/libc/syscall.cpp) 的封装里)是给编译器的承诺:`sys_exit` 不会返回,别在后面排什么清栈指令。

第二步,`objcopy -O binary` 把 ELF 抽成 flat binary(剥掉 ELF 头,只留可执行字节,加载到 `0x400000` 就能跑)。第三步最巧妙:用 `ld -r -b binary hello.bin` 把这个 flat binary 包成一个可链接的 `.o`,它会自动生成 `_binary_hello_bin_start` / `_binary_hello_bin_end` 两个符号。[usermode.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/usermode.cpp) 就靠这两个符号把用户程序字节逐个拷进分配好的用户代码页:

```cpp
extern const uint8_t _binary_hello_bin_start[];
extern const uint8_t _binary_hello_bin_end[];
// ...
size_t user_size = _binary_hello_bin_end - _binary_hello_bin_start;
auto* code_virt  = reinterpret_cast<uint8_t*>(code_phys + KERNEL_VMA);
for (size_t i = 0; i < user_size; i++) code_virt[i] = _binary_hello_bin_start[i];
```

用户态那侧的 `syscall` 封装也是手写的:[syscall.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/libc/syscall.cpp) 的 `_syscall3` 是一段内联汇编,把号塞 `rax`、参数塞 `rdi/rsi/rdx`,执行 `syscall`,clobber 列里老老实实写上 `rcx`、`r11`、`memory`——因为 SYSCALL 会破坏 rcx(存了 RIP)和 r11(存了 RFLAGS),不声明 clobber,编译器会以为这俩寄存器跨调用不变,优化出灾难。

## 顺带打通的 FPU/SSE 与栈对齐

这一块不是 syscall 的本职,但没有它,`hello.cpp` 根本跑不到 `sys_write` 那一行。根因是 GCC 会把 `const char msg[] = "..."` 的初始化优化成 SSE 的 `movaps`——而 `movaps` 要求目标 16 字节对齐,不对齐就 `#GP`。

打开 FPU 是 [boot.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/boot.S) 的事:置 CR4 的 OSFXSR(bit9)+ OSXMMEXCPT(bit10)、清 CR0 的 EM(bit2)、置 MP(bit1)、`clts` 清 TS。这告诉 CPU「操作系统支持 SSE、会用 FXSAVE/FXRSTOR 保存 SSE 状态、别替我仿真 x87」。配套地,`Task` 结构体加了 `alignas(16) uint8_t fpu_state[512]`——FXSAVE 恰好写 512 字节、且目标必须 16 字节对齐否则自身就 #GP;`TaskBuilder::build()` 里 `fninit + fxsave` 给每个任务初始化一份干净状态;[scheduler.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/scheduler.cpp) 的 `schedule`/`exit_current`/`run_first` 三处都在 `context_switch` 前后配 `fxsave`(存当前)/`fxrstor`(恢复下一个),保证任务切换时 SSE 状态跟着走。

光开 FPU 还不够——`movaps` 仍然 #GP,真因是栈不满足 SysV 对齐。ABI 要求函数入口 `RSP ≡ 8 mod 16`(模拟 `call` 压入 8 字节返回地址后栈 16 对齐)。可 Cinux 的 `USER_STACK_TOP = 0x7FFFFF000` 本身是 `0 mod 16`,`sub rsp,0x28` 之后变成 `8 mod 16`,`movaps` 当场炸。修复是在跳转前把 RSP 减 8,并锁一道编译期断言:

```cpp
constexpr uint64_t USER_ABI_RSP_OFFSET = 8;
static_assert((USER_STACK_TOP - USER_ABI_RSP_OFFSET) % 16 == 8,
              "User entry RSP must satisfy x86_64 ABI alignment");
// launch_first_user 里:
jump_to_usermode(USER_ENTRY_BASE, USER_STACK_TOP - USER_ABI_RSP_OFFSET, 0);
```

ABI 对齐是编译期就该锁死的契约,不该等运行时 `#GP` 了才发现。这道 `static_assert` 把它焊死。
