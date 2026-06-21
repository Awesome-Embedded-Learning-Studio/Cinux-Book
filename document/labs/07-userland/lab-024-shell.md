---
title: Lab 024 · 给内核一个能对话的用户态:shell
---

# Lab 024 · 给内核一个能对话的用户态:shell

> 配套章节:[024 · 给内核一个能对话的用户态:shell](../../book/07-userland/024-shell.md)。这一关给你目标和约束,不贴 `shell/main.cpp` 的完整主循环、不贴 `tokenize` 的成品实现、不贴 `cmd_clear` 那串转义、更不贴 `sys_read` 全文和 `syscall.S` 的出口段——那些得你自己写、自己把两颗 SYSCALL/SYSRET 路径上的雷拆出来。

## 实验目标

023 把用户态的 `hello` 送进 Ring 3、让它能调 `sys_write` 打一行字再走人。024 要把它从一个「只会喊一句话」的程序,升级成一个能读键盘、解析命令、把命令分发到具体处理器的**交互式 REPL shell**。拆成几个能独立验证的子目标:

1. **逐字符读一行**:`read_line` 经 `sys_read(0, &c, 1)` 一个字符一个字符地读,边读边回显;遇到退格(`0x7F` 或 `\b`)要把上一个字符擦掉;遇到换行收尾、NUL 终止,且不把换行本身存进缓冲。
2. **就地分词**:`tokenize` 按空格和 Tab 把一行命令切成 token,返回 `argc` 和 `argv`,**不改名、不分配**,直接在原缓冲上把空白变 NUL。
3. **数据驱动的命令表**:`builtin_cmds[]` 是个以 `{nullptr, nullptr}` 哨兵结尾的 `CmdEntry` 数组,主循环靠它查表分发,加命令只动表和写个新 `cmd_xxx`。
4. **三个内置命令**:`echo` 把 `argv[1..]` 用单空格连起来加换行;`help` 打命令清单;`clear` 发 7 字节 `\033[2J\033[H` 清屏归位。
5. **内核侧 `sys_read`**:只认 `fd=0`,从 `Keyboard::poll` 取事件,把 `\r` 转成 `\n`,遇 `\n` 停(一次喂 shell 一整行);空缓冲时 `pause` 自旋等第一个字符。
6. **把 `hello` 换成 `shell`**:`launch_first_user` 嵌入的二进制从 `_binary_hello_bin_*` 换成 `_binary_shell_bin_*`,ELF 入口是 `_start`,跑完调 `sys_exit(0)`。

做完这几条,内核就第一次有了「能跟人对话」的用户态。但说在前面:这个 shell **没有文件系统、没有命令历史、没有管道、没有重定向**——就 `echo/help/clear` 三个命令、一个死循环 REPL。它是把「Ring 3 → 内核 → Ring 3」这条往返路真正用起来,顺便把 GDT 重排和两处 SYSRETQ 出口修正落到代码里。

## 前置条件

你得先过 Lab 022(usermode)和 Lab 023(syscall)。关键依赖:

- **022 的 Ring 3 跳板**:`launch_first_user` 会建一个用户地址空间、映射一页代码到 `USER_ENTRY_BASE`(0x400000)、映射 `USER_STACK_PAGES` 页用户栈、设置 `TSS.RSP0`、布置 GS base 页、然后用 `jump_to_usermode` 经 SYSRET 跳进 Ring 3。这一关你要把「嵌入哪段二进制」从 hello 换成 shell。
- **023 的 SYSCALL 基础设施**:`syscall_init()` 配 LSTAR/STAR/SFMASK、把 `g_syscall_kernel_rsp` 捕获好、注册 `SYS_read/write/exit/yield` 四个 handler;`syscall_entry`(在 `syscall.S`)负责 swapgs、换内核栈、建 trap frame、调 `syscall_dispatch`、恢复现场、sysretq 回 Ring 3。
- **键盘与控制台**:PIT(100 Hz)和键盘(IRQ1)已在 022/023 的 `main` 里 unmask 并 `sti`;`Keyboard::poll(KeyEvent)` 能从 PS/2 环形缓冲取事件;`Console::putc` 是串口 + 屏幕双输出的落脚点。
- **用户态的 freestanding 头**:`<cstddef>` / `<cstdint>` 可用,但**没有 libstdc++、没有 malloc、没有 `<string.h>`**——`strlen/strcmp/memset/memcpy/memcmp` 你要在 `user/libc/string.cpp` 里自己写。

还得理解一个外部约定:**SYSCALL/SYSRETQ 只自动保存 RCX 和 R11**(RCX 存返回 RIP、R11 存 RFLAGS),**其余所有通用寄存器都靠软件保存恢复**。SYSCALL 入口栈帧里那一堆 `pushq` 就是为了把 RBX/RBP/R9..RDI 全压下来,出口再全部恢复。这条约定是这一关第二颗雷的根因——别拿 callee-saved 的 RBX 当 scratch。

## 任务分解

**第一步:用户态字符串与 libc(`user/libc/string.{hpp,cpp}`)。** 先把地基铺好:`cinux::user::strlen/strcmp/memset/memcpy/memcmp`,全部 freestanding、逐字节循环。`strcmp` 返回首个不同字节的差(相等返 0);`memset`/`memcpy`/`memcmp` 处理 `n=0` 边界、返回 `dest`。想清楚为什么 shell 一定要这一层——`cmd_echo` 要 `strlen(argv[i])` 才知道写多少字节、`main` 里查表要 `strcmp(argv[0], name)`。没有它,你连「echo」这个词都比不了。

**第二步:`read_line`——逐字符读 + 回显 + 退格。** 签名 `size_t read_line(char* buf, size_t cap)`。循环里每次 `sys_read(0, &c, 1)` 读一个字符(返回值 `<= 0` 就 `continue`,别崩)。三种字符分支:换行 `\n` 先 `write_buf("\n", 1)` 回显个回车再 `break`(注意**不把 `\n` 写进 buf**);退格(`0x7F` 或 `\b`)在 `pos > 0` 时 `--pos` 然后 `write_buf("\b \b", 3)`——这三字节是「光标左移一格、用空格盖掉旧字符、再左移回来」,少发一个屏幕上都擦不干净;普通字符先 `write_buf(&c, 1)` 回显、再 `buf[pos++] = c`。循环条件 `pos < cap - 1` 给末尾的 NUL 留一位,出口 `buf[pos] = '\0'`、返回 `pos`。想清楚为什么**逐字符读而不是一次读一行**:内核侧 `sys_read` 现在只会在收到 `\n` 时才返回一整行,但 shell 必须在每个字符到达时就回显——所以你只能一字一读。

**第三步:`tokenize`——就地切割。** 签名 `size_t tokenize(char* line, char** argv, size_t max_tokens)`,返回 `argc`。逻辑:外层 `while (*line != '\0' && argc < max_tokens)`,内层先跳前导空格和 Tab、跳完若已到末尾就 `break`;记下 `argv[argc++] = line`(指向当前 token 起点);再往前走直到遇空白或末尾;若停在空白字符上,就**就地把它写成 NUL** 再 `++line`。想清楚两个点:(a) 为什么**不 malloc**——shell 是 freestanding、没有堆,token 指针只能指向原缓冲内部的地址;(b) 为什么要 `max_tokens` 截断——`argv` 数组是定长的(`MAX_TOKENS = 16`),用户乱敲一长串命令不能让它越界。

**第四步:`CmdEntry` 哨兵表 + 三个命令。** 在 `shell.hpp` 定义:

```text
struct CmdEntry {
    const char* name;
    void (*handler)(int argc, char** argv);
};
```

`cmd_echo(int argc, char** argv)`:从 `i = 1` 起,每个参数之间补一个单空格(`i > 1` 时先 `sys_write(1, " ", 1)`),用 `write_str(argv[i])` 输出,最后补一个 `\n`。`cmd_help` 打三行命令清单(固定字符串)。`cmd_clear` 只有一句:`sys_write(1, "\033[2J\033[H", 7)`——7 字节,`ESC[2J` 全屏擦除、`ESC[H` 光标归位。主循环里 `builtin_cmds[]` 就是 `{"echo", cmd_echo}, {"help", cmd_help}, {"clear", cmd_clear}, {nullptr, nullptr}`,遍历到 `name == nullptr` 停;命中就调 `handler(argc, argv)` 并 `break`,全没命中就打「command not found」。**这一关的 shell 只有这三个命令**;别照着 Linux 的 shell 脑补 `cat/ls/cd/重定向`——那些要等文件系统(025 以后)。

**第五步:内核侧 `sys_read`(`kernel/syscall/sys_read.cpp`,全新)。** 签名照 `SyscallFn`:`int64_t sys_read(uint64_t fd, uint64_t buf_virt, uint64_t count, uint64_t, uint64_t, uint64_t)`。入口两道守卫:`buf_virt >= 0x800000000000`(用户地址上限)返回 -1;`fd != 0` 返回 -1(只认 stdin)。主体是个 `while (read_bytes < count)` 循环:先 `Keyboard::poll(ev)` 取事件,取不到时——若已经有数据(`read_bytes > 0`)就 `break` 把已读的返回,否则 `pause` 自旋等 `SPIN_WAIT_ITERS`(常量)次直到取到第一个字符。取到的事件只收 `pressed && ascii != 0` 的;把 `'\r'` 转成 `'\n'`;写进缓冲;遇 `'\n'` 立即 `break`(保证一次给 shell 一整行)。想清楚为什么**只认 fd=0**:这一关没有文件系统、没有别的 fd;为什么**自旋而不阻塞**:024 还是单任务、没有可阻塞唤醒的调度路径(阻塞唤醒是 021 的能力,但 shell 这条路不接它),`pause` 自旋是最朴素的「等键盘」。

**第六步:把 hello 换成 shell。** 023 的 `launch_first_user` 嵌的是 `_binary_hello_bin_*`;024 把这两个 `extern` 符号、以及拷贝循环用的 `_end - _start` 换成 `_binary_shell_bin_*`。二进制怎么来:`user/CMakeLists.txt` 用 `add_executable(user_shell ...)` 编 `main.cpp` + 三个 `cmd_*.cpp`、链 `user_libc`、`objcopy -O binary` 剥成 `shell.bin`、再用 `ld -r -b binary` 包成 `user_binary.o`(产生 `_binary_shell_bin_start/_end` 符号),内核链接时吃进去。`main.cpp` 里那一行 `[BIG] ===== Milestone 023 =====` 是**遗留字符串、本关没改它**,别被它误导——真正说明「跑到 shell」的是串口里冒出的 `Cinux shell - type 'help' for commands`。

**第七步(顺带、必做):GDT 重排 + STAR 改 0x23。** 这一步的动机不在 shell 逻辑本身,而是让 SYSRETQ 这条回路在 QEMU 上不炸。把 GDT 从「5 段 + TSS」重排成 9 项:NULL、TLS 占位(idx1)、KernelCode(0x10)、KernelData(0x18)、User32Code(0x20)、UserData(0x28)、User64Code(0x30)、TSS(0x38,两槽)。选择子常量:`GDT_KERNEL_CODE=0x10`、`GDT_KERNEL_DATA=0x18`、`GDT_USER_CODE=0x33`、`GDT_USER_DATA=0x2B`、`GDT_TSS=0x38`,新增 `GDT_SYSRET_BASE=0x23`。`syscall_init` 里 `STAR = (GDT_SYSRET_BASE << 48) | (GDT_KERNEL_CODE << 32)`;`usermode.S` 里 STAR 立即数也改成 `$0x23`。为什么要 0x23 而不是 0x20——见下面「调试现场」第一条,这是 QEMU 行为逼出来的修正。

## 接口约束

你要实现出来的东西,对外长这样(职责与签名,不给实现):

- 用户态 libc(`user/libc/string.hpp`,`namespace cinux::user`):`size_t strlen(const char*)`、`int strcmp(const char*, const char*)`、`void* memset(void*, int, size_t)`、`void* memcpy(void*, const void*, size_t)`、`int memcmp(const void*, const void*, size_t)`。
- 用户态 syscall 封装(`user/libc/syscall.h`):`int64_t sys_read(int, void*, size_t)`、`int64_t sys_write(int, const void*, size_t)`、`void sys_exit(int)`、`void sys_yield(void)`。**这一关用户态只有这四个 syscall**,没有 `sys_open/creat/close`。
- `struct CmdEntry { const char* name; void (*handler)(int argc, char** argv); };`(`shell.hpp`)。
- `size_t read_line(char* buf, size_t cap)`、`size_t tokenize(char* line, char** argv, size_t max_tokens)`(都在 `main.cpp` 的匿名命名空间)。
- `void cmd_echo(int argc, char** argv)`、`void cmd_help(int argc, char** argv)`、`void cmd_clear(int argc, char** argv)`(各一个 `.cpp`)。
- 内核 `int64_t sys_read(uint64_t fd, uint64_t buf_virt, uint64_t count, uint64_t, uint64_t, uint64_t)`(`kernel/syscall/sys_read.{hpp,cpp}`),`SYS_read` 编号注册为 0。

关键约束(违反就翻车):

- **`read_line` 的退格必须发 `\b \b` 三字节**,少发一个屏幕都擦不干净(光标不会回退到位 / 残影留着)。换行**不写入** buf,只回显。
- **`tokenize` 必须就地改写** `line`(把空白替换成 NUL),`argv[i]` 指向缓冲内部;`argc` 不能超过 `max_tokens`。
- **`_start` 跑完必须 `sys_exit(0)`**。ELF 入口是 `_start` 不是 `main`/`shell_main`;shell 主循环是死循环理论上不返回,但入口契约要求兜底 `sys_exit`,否则主循环若被意外跳出会 `ret` 进未定义地址。
- **`sys_read` 的两道守卫**(`buf_virt >= 0x800000000000`、`fd != 0`)一个都不能漏,前者防用户程序拿内核地址当读缓冲越权,后者保证只走 stdin。
- **`builtin_cmds[]` 必须以 `{nullptr, nullptr}` 收尾**;遍历分发靠这个哨兵停。加命令的扩展点就在这张表。
- **syscall 出口不许拿 callee-saved 寄存器当 scratch**。SYSCALL 只自动存 RCX/R11,RBX/RBP 是 callee-saved,碰了用户的 RBX,shell 全瘫(见下面调试现场二)。返回值要暂存就存到 GS scratch 区(`gs:16`),出口从 trap frame `rsp+80` 把用户 RBX 恢复回去。

汇编出口具体怎么排、GDT 描述符 access/flags 怎么编码、STAR 立即数怎么移位、`sys_read` 自旋次数取多少——这些**这一关不提供成品**,你自己定,但定下来就得和 `GDT_SYSRET_BASE` 常量、和 `syscall_init` 写进 STAR 的值逐位对齐。

## 验证步骤

shell 的纯逻辑(字符串工具、tokenizer、`CmdEntry` 分发、`cmd_echo/cmd_help/cmd_clear` 的输出、`read_line` 的退格/换行)在 host 上镜像着测——把这些纯逻辑在 `test/unit/test_shell.cpp` 里重写一份(不链内核、不跑汇编),`-O2` 编、`CINUX_HOST_TEST` 门控;`sys_write/sys_read` 用 mock 替掉。建议覆盖:`strlen/strcmp/memset/memcpy/memcmp` 的全边界(空串、等/不等、零长度、首/末字节差异)、`tokenize`(单词、多词、首尾空白、Tab、`max_tokens` 截断、空串、纯空白)、`CmdEntry` 查表命中/未命中/哨兵计数、`cmd_echo`(单参/多参/无参)、`cmd_help`(非空输出)、`cmd_clear`(发对 7 字节 `\033[2J\033[H`)、`read_line`(退格回退、换行终止不存):

```bash
ctest --test-dir build -R shell --output-on-failure
```

真正的内核侧基础设施(`sys_write/sys_read` 的 fd/地址守卫、`SyscallNr` 常量、GDT 选择子、STAR/SFMASK 的 MSR 回读)只能在 QEMU 里验。机内测在 `kernel/test/test_shell.cpp` 里(节名 `Shell Tests (024)`),它说明一件重要的事:**真用户态 shell 跑在 Ring 3,机内测无法直接调它**,只能验它依赖的内核基础设施。配合 `test_syscall.cpp`/`test_usermode.cpp`/`test_gdt_idt.cpp` 把「GDT 重排 + STAR 改 0x23」钉死——尤其 STAR[47:32]=0x10、STAR[63:48]=0x23、`GDT_USER_CODE/USER_DATA` 带 RPL=3 这几个回读断言,是这一关的回归护栏:

```bash
cmake --build build --target run-big-kernel-test
```

最后跑**生产内核本身**(直接跑大内核的 QEMU 目标),串口应依次看到:`launch_first_user` 的一串 `[USER] ...` 初始化日志、`Jumping to Ring 3`,然后用户态打出 `Cinux shell - type 'help' for commands`,接着出现 `cinux> ` 提示符。这时(若接了交互输入)敲 `help` 看到三行命令清单、敲 `echo hello world` 看到 `hello world`、敲 `clear` 看到屏幕被擦净光标归位、退格能删字——这几样齐了,shell 就是「能对话」的了。

## 常见故障

这一关踩的雷大多是「现象像 shell 的 bug、根因在 SYSCALL 路径」。两条都是真事,各拆成 症状 → 根因 → 修复 → 防复发:

- **症状:shell 起来打完 prompt,PIT 一 tick 就 `#GP`,错误码 `0x28`。** 错误码 `0x28` = GDT 第 5 项(User Data)的 selector,RPL=0——也就是说 `iretq` 想把用户态 SS 加载成 `0x28`,但目标 CPL=3,SS 的 RPL/DPL 检查不过。根因不是你 GDT 错(描述符 DPL=3 是对的)、也不是 STAR 错(回读 `0x00200010` 是对的),而是**QEMU/TCG 在 SYSRETQ 算 SS selector 时没执行 SDM 里的 `OR 3`**(CS 它倒是执行了,所以 CS=0x33 对、SS=0x28 错)。诊断办法:在 PIT 的 C handler 里打 `frame->cs`/`frame->ss`,会看到用户中断时 `SS=0x0028`。修复:别指望 SYSRETQ 帮你设 RPL,把 RPL=3 编码进 STAR 基值——用 `GDT_SYSRET_BASE = 0x23`,这样 `+8 = 0x2B`、`+16 = 0x33` 本身就带 RPL=3。防复发:**别信 SYSRETQ 出来的 SS.RPL**,基值自带 RPL 最稳;注意这个根因必须说成「QEMU 行为」,Intel SDM 明确写 SYSRETQ 会 `(...+8) OR 3`,真硬件是对的。

- **症状:回显一切正常,但 `echo hello` 打不出 `hello`、`clear` 也不清屏——所有命令全失效。** 这种「共性失效」别去逐个命令查,直接怀疑 syscall 路径本身。根因:`syscall.S` 出口用 `movq %rax, %rbx` 把返回值暂存到 RBX,破坏了用户的 RBX——编译器把 `read_line` 里的 `pos` 变量分配进了 RBX(callee-saved,适合跨函数调用存活),结果每次 `sys_read`/`sys_write` 返回后 `pos` 都被覆盖成返回值 1,每个字符都写到 `line[1]`、回车时 `line[1] = 0`,最终 `line` 是空串、tokenize 出 0 个 token。诊断办法:在内核各层加 debug 打印(顺手会逼出一个用户态 `printf`,因为 `kprintf` 不在用户态可用),看到 `line='' len=1` 的怪象;再反汇编 `user_shell` 看到 `lea rdx, [rbx+0x1]`、反汇编 `syscall.S` 看到 `movq %rax, %rbx`,两头一对就锁死。修复:返回值改存 GS scratch 的 `gs:16`,出口从 trap frame `rsp+80` 把用户 RBX 恢复回去。防复发:**SYSCALL 只自动存 RCX/R11,其余全靠软件;RBX/RBP/R12–R15 是 callee-saved,绝不能拿来当 scratch**。

- **回显正常、命令也匹配,但 `clear` 按了没反应(屏幕不净)。** 多半是内核侧 `Console` 没吃 ANSI CSI:`cmd_clear` 发的是 `\033[2J\033[H`,如果 `putc` 直接把 `ESC`、`[`、`2`、`J` 当普通字符渲染,屏幕上就冒出一串乱码而不是清屏。检查 `Console::putc` 前面是不是挂了一台 `Normal/Esc/Bracket` 三态状态机,终止字节 `J`(参数 2)是不是真的调到 `clear()`、`H` 是不是把光标归位。

- **`tokenize` 漏词或把多个词粘成一个。** 要么没跳 Tab(只跳了空格)、要么就地 NUL 写错位置(在空白处没写 NUL 导致下一个 token 的起点算错)。注意分词要把空格**和 Tab** 都当分隔符;停在空白字符时要 `*line++ = '\0'` 把它断开。

- **`echo` 多参数之间没有空格,或末尾多了个空格。** `cmd_echo` 里分隔空格的条件要写成 `i > 1`(从 argv[1] 起算,第二个参数之前才补空格),写成 `i > 0` 就会在 `echo` 后面先多打一个空格。

- **`sys_read` 一调就返回 -1,shell 读不到任何字符。** 检查 fd 是不是传成了 1(写 fd)而不是 0(读 fd);或者 `buf_virt` 越过了 `0x800000000000` 上限被守卫挡了(用户栈在 `0x7FFFFF000` 一带,正常不会越,但若你把缓冲放到了奇怪地址就会触发)。

## 通过标准

1. host 单测全绿:`strlen/strcmp/memset/memcpy/memcmp` 全边界、`tokenize`(单/多词、首尾空白、Tab、`max_tokens` 截断、空串/纯空白)、`CmdEntry` 分发命中/未命中/哨兵计数、`cmd_echo`(单/多/无参)、`cmd_help`(非空输出)、`cmd_clear`(发对 7 字节转义)、`read_line`(退格、换行终止不存)。
2. QEMU 机内测通过,节 `Shell Tests (024)` 全绿,且 `test_syscall`/`test_usermode`/`test_gdt_idt` 里 STAR[47:32]=0x10、STAR[63:48]=0x23、`GDT_USER_CODE=0x33`/`GDT_USER_DATA=0x2B` 带 RPL=3 的回读断言过。
3. 串口出现 `Cinux shell - type 'help' for commands` 与 `cinux> ` 提示符;`help` 打三行命令清单;`echo <args>` 把参数用单空格连起来输出并换行;`clear` 清屏并光标归位;退格能正确删字(`\b \b` 三字节齐全)。
4. `sys_read` 只认 `fd=0`、`buf_virt < 0x800000000000`,从 `Keyboard::poll` 取事件、`\r→\n`、遇 `\n` 停(一次一行),空缓冲时 `pause` 自旋。
5. SYSRETQ 出口的 CS=0x33 / SS=0x2B 都带 RPL=3(STAR 基值用 0x23 自带 RPL),PIT 中断往返不再触发 `#GP(0x28)`。
6. `syscall_entry` 出口不破坏用户 callee-saved:返回值走 `gs:16`、用户 RBX 从 trap frame `rsp+80` 恢复——shell 所有命令正常工作、`echo hello` 真的打出 `hello`。

做到这六条,内核就第一次有了「能跟人对话」的用户态。但这个 shell 只在内存里转——它读的是键盘、写的是屏幕,没有任何东西落盘。下一站 025 接 AHCI/PCI,把数据真正写到 SATA 盘上,那才会带来真正的文件系统命令(`cat/ls/...`),也才会让 shell 从「会说话」变成「会管文件」。
