---
title: 024 · 给内核一个能对话的用户态:shell
---

# 024 · 给内核一个能对话的用户态:shell

> 上一章(023)我们把那条「用户态 → 内核」的服务通道接通了:`sys_write/exit/yield` 三个号能通,一行 `[USER] Hello from Ring 3!` 真的是 Ring 3 自己请求内核打出来的。可那个用户程序 `hello` 打完一行字就 `sys_exit(0)` 走人了,从不读键盘、也不解析命令——说到底它只是个「会说话的留声机」。这一章把它换成一个**能对话**的东西:一个最小的 REPL shell,常驻 Ring 3,读键盘、回显、退格、按回车收一行、就地切成参数、查表派发命令,再调回内核干活。顺带拆两颗 SYSCALL/SYSRET 路径上非常真实的雷——一颗让 shell 刚冒头就被时钟中断炸成 #GP,一颗让所有命令集体失声。做完,串口里会出现一句 `Cinux shell - type 'help' for commands` 和一个 `cinux> ` 提示符,你敲 `echo hello`,它老老实实回你 `hello`。

## 这一章我们要点亮什么

核心是一件:让用户态从「单向输出、一锤子买卖」升级成「能读、能解析、能常驻」的交互式程序。

具体说,024 交付六块:

- **shell REPL 主体**([main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/programs/shell/main.cpp)):`_start → shell_main` 一个死循环,`print_prompt → read_line → tokenize → dispatch`;`read_line` 经 `sys_read(0,&c,1)` 逐字符读、回显、处理退格(`0x7F`/`\b` 时发三字节 `\b \b`)、遇 `\n` 收尾(换行本身不存进缓冲区)。ELF 入口是 `_start`,跑完调 `sys_exit(0)`。
- **sys_read 处理器**([sys_read.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/sys_read.cpp),全新):fd 只认 0(stdin),从 `Keyboard::poll` 取 `KeyEvent`,只收 `pressed && ascii!=0`,把 `\r` 转成 `\n`,遇 `\n` 就停(一次给 shell 一整行);空缓冲时 spin-wait 等第一个字符。`SYS_read=0` 正式注册进分发表。
- **用户态 libc**([string.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/libc/string.hpp)、[printf.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/libc/printf.hpp)):`cinux::user::strlen/strcmp/memset/memcpy/memcmp`(freestanding,给 shell 做字符串比较和命令分发的底座);`printf` 支持 `%c %s %d %u %x %p %%` 加 `%l`/`%ll` 长度修饰。
- **GDT 重排成 Linux 兼容**([gdt.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/gdt.hpp)):从「5 段 + TSS = 7 项」改成「NULL + idx1 TLS 占位 + 内核 CS/DS + 用户 32 位 CS + 用户 DS + 用户 64 位 CS + TSS」共 9 项。选择子常量随之全改:`GDT_KERNEL_CODE=0x10`、`GDT_KERNEL_DATA=0x18`、`GDT_USER_DATA=0x2B`、`GDT_USER_CODE=0x33`、`GDT_TSS=0x38`,并新增 `GDT_SYSRET_BASE=0x23`。
- **两处 SYSCALL/SYSRET 出口修正**:其一是 `GDT_SYSRET_BASE` 取 0x23 而非 0x20,让 SYSRETQ 的 `+8`/`+16` 直接算出带 RPL=3 的 0x2B/0x33;其二是 `syscall.S` 出口不再用 RBX 暂存返回值、改用 `gs:16` scratch,并从 trap frame `rsp+80` 恢复用户 RBX。这两处都是被真实崩溃逼出来的,调试现场会展开。
- **Console 吃 ANSI CSI**([console.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/video/console.hpp)):`putc` 前置一台三态状态机(`Normal/Esc/Bracket`),识别 `ESC[` 后收参数字节和终止字节再派发,实现 `ESC[2J`(全屏擦除)和 `ESC[H`(光标归位)。这是 `cmd_clear` 那串转义能真清屏的内核侧落脚点。

合起来,这一章让内核第一次拥有一个「常驻 Ring 3、和用户来回交互」的程序。但期望要放正:024 的 shell **只有三个命令**——`echo`、`help`、`clear`,`shell.hpp` 只声明这三个,`builtin_cmds[]` 也只注册这三个。没有 `cat/ls/touch/mkdir/rm/cd/pwd` 这些文件系统命令,没有输出重定向 `>`,没有命令历史、管道、通配、环境变量、作业控制,更没有文件系统——用户态的 `syscall.h` 只有 `sys_read/sys_write/sys_exit/sys_yield` 四个号。它就是一个死循环 REPL,键盘进来一行、查表派一下、再回来等下一行。`echo hello` 能回 `hello`,`clear` 能清屏,仅此而已。

还有一处容易误读的遗留痕迹得说清:[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp) 里那行 `[BIG] ===== Milestone 023 =====` 的横幅**本章没改**,它是 023 留下的字符串;真正说明「跑到 shell」的不是它,而是串口里冒出来的 `Cinux shell - type 'help' for commands`。同样,[usermode.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/usermode.hpp) 的注释里还写着「CLI 触发 #GP」(以及「user code/data selectors at 0x1B/0x23」)——那是 022 用 4 字节死程序验证隔离时的描述,024 实际嵌入和跳转的是 `shell.bin`,注释没跟着改,别被它带偏。

## 为什么现在需要它

先看 023 的痛在哪儿。`hello` 这个程序只会 `sys_write` 一段问候、然后 `sys_exit(0)`——它**单向**。用户想跟它交互,它根本不读键盘:那时候 `SyscallNr::SYS_read=0` 这个常量虽然在,但内核侧没人接它,dispatch 到它就返回 `-1`。所以 024 的第一块地基,就是把 `sys_read` 这个号真的接上、让它能从键盘环形缓冲里取字符送回 Ring 3。

接上之后,朴素到近乎寒酸的设计选择就来了:shell 的 `read_line` 为什么是**逐字符** `sys_read(0,&c,1)` 地读,而不是一上来 `sys_read` 一整行?因为 024 的 `sys_read` 还是个非常原始的处理器——它只认 `fd==0`、空了就 spin-wait,没有缓冲区、没有非阻塞、没有 select。逐字符读,shell 才能在每个字符到达时立刻回显、并在遇到退格时当场擦掉,而不是等用户敲完整行才一口气显示。这种「内核只负责把键盘字符逐个递上来、行编辑(回显、退格、换行收尾)全部在用户态做」的分工,是最省事的:内核不用懂「行」是什么概念,shell 自己懂。

`tokenize` 为什么是**就地**把空格改成 NUL、返回 argv 指针数组,而不是 `malloc` 一份份拷贝?因为 024 的用户态**没有堆**——`user_libc` 里只有 `string` 和 `printf`,没有 `malloc/free`。就地切割省掉了内存分配这一整摊事:输入行 `line[]` 本来就是 shell 栈上的一个数组,`tokenize` 只是把里面的空格和 Tab 写成 `\0`、再把每个词的起始地址记进 `argv[]`。命令名比较靠 `cinux::user::strcmp`,一字一字比到 `\0` 为止。

至于为什么在这个节骨眼上**顺带把 GDT 整个重排成 Linux 那套**——这不是洁癖,是被 SYSRETQ 逼的。023 那版 GDT 布局下,SYSRETQ 返回 Ring 3 时算出的 CS/SS 在我们这台 QEMU 上会出岔子(下面调试现场详谈)。重排后的布局让 `STAR[63:48]=0x23` 时,`+8`/`+16` 直接落在 `0x2B`/`0x33` 这两个**自带 RPL=3** 的选择子上,把对「CPU/模拟器到底给不给 SS 加 RPL」的依赖彻底消除。这是为 shell 能稳定地来回穿越 Ring 3/0 铺路。

## 设计图

先看 shell 主循环的节奏,以及它和内核 `sys_read` 的配合:

```text
   Ring 3: shell_main (死循环)                        Ring 0: sys_read 处理器
   ┌───────────────────────────┐                    ┌──────────────────────────┐
   │ print "cinux> "           │                    │ fd==0 && buf<0x8000_0000_0000 守卫│
   │ read_line(line,256):      │                    │                          │
   │   loop:                   │  sys_read(0,&c,1)  │   Keyboard::poll(ev)     │
   │     ─────────────────────►├───────────────────►│   空了? spin-wait(1M×pause)│
   │     c = ◄─────────────────┤◄───────────────────│   \r → \n; 非 pressed/ascii=0 丢│
   │     if 0x7F/\b: pos--,    │                    │   遇 \n: 存入 buf 后停  │
   │        发 "\b \b"(3字节) │                    │   返回本次读到的字节数  │
   │     else: 回显 c, line[pos++]=c│               │                          │
   │     if '\n': 发 "\n", 跳出│                    └──────────────────────────┘
   │ line[pos]='\0'            │
   │ tokenize(line,argv,16)    │  ── 就地把 ' '/'\t' 改成 '\0', argv[i] 指向各词
   │ dispatch: 遍历 builtin_cmds[] ── strcmp(argv[0], name) ── 命中则 handler(argc,argv)
   └───────────────────────────┘
```

再看 `syscall_entry` 在内核栈上搭的 trap frame——它是汇编和 C 之间的契约,`rsp+80` 那一格是 024-02 修复的关键:

```text
   syscall_entry 的 trap frame (内核栈, 压栈顺序自下而上)
   ┌─────────────────────────────────────┐
   │ rsp+0:   user RSP   (从 gs:8 取回)    │
   │ rsp+8:   user RIP   (= RCX)           │  SYSRETQ 从 RCX 加载 RIP
   │ rsp+16:  user RFLAGS(= R11)           │  SYSRETQ 从 R11 加载 RFLAGS
   │ rsp+24:  syscall nr (RAX)             │
   │ rsp+32:  arg1 RDI                     │
   │ rsp+40:  arg2 RSI                     │
   │ rsp+48:  arg3 RDX                     │
   │ rsp+56:  arg4 R10                     │
   │ rsp+64:  arg5 R8                      │
   │ rsp+72:  arg6 R9                      │
   │ rsp+80:  callee-saved RBX  ◄────────── 024-02 修复: 出口必须从这里恢复用户 RBX
   │ rsp+88:  callee-saved RBP             │
   └─────────────────────────────────────┘
        per-CPU GS scratch (一块 4KB 页):
          gs:0  = 内核 RSP (syscall 入口载入用)
          gs:8  = 用户 RSP 暂存槽 (入口存、出口取)
          gs:16 = 返回值暂存槽   ◄── 024-02 修复: 返回值改存这里, 不再用 RBX
```

最后是重排后的 GDT 9 项布局,以及 `STAR[63:48]=0x23` 如何算出 SYSRETQ 的 CS/SS:

```text
   GDT (9 项, kEntryCount=9)
   idx  sel    内容
    0  0x00   NULL
    1  0x08   占位 (TLS 预留, 现为 null_entry)
    2  0x10   Kernel Code  ◄── STAR[47:32]=0x10, SYSCALL CS
    3  0x18   Kernel Data  ◄── SYSCALL SS = 0x10+8 = 0x18
    4  0x20   User32 Code (Size32, DPL3)
    5  0x28   User Data    ◄── SYSRETQ SS 目标: 0x23+8 = 0x2B (RPL=3 已编进基值)
    6  0x30   User64 Code  ◄── SYSRETQ CS 目标: 0x23+16 = 0x33 (RPL=3 已编进基值)
    7  0x38   TSS low  ┐ 16 字节, 跨两槽
    8  0x??   TSS high ┘

   STAR = (GDT_SYSRET_BASE << 48) | (GDT_KERNEL_CODE << 32)
        = (0x23 << 48)        | (0x10 << 32)

   SYSCALL 入口 (读 STAR[47:32]):  CS = 0x10,  SS = 0x10+8 = 0x18   ✓ 内核态
   SYSRETQ 出口 (读 STAR[63:48]):  CS = 0x23+16 = 0x33,  SS = 0x23+8 = 0x2B  ✓ 用户态, RPL 自带
```

## 代码路线

### shell_main 与 read_line:一个逐字符的 REPL

[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/programs/shell/main.cpp) 的主循环没什么花架子,就是个 `while (true)`:

```cpp
write_str("Cinux shell - type 'help' for commands\n");
while (true) {
    write_str(PROMPT);                       // "cinux> "
    size_t len = read_line(line, MAX_LINE);
    if (len == 0) continue;                  // 空行直接重来
    size_t argc = tokenize(line, argv, MAX_TOKENS);
    if (argc == 0) continue;                 // 全空白也重来
    bool found = false;
    for (size_t i = 0; builtin_cmds[i].name != nullptr; ++i) {
        if (strcmp(argv[0], builtin_cmds[i].name) == 0) {
            builtin_cmds[i].handler(static_cast<int>(argc), argv);
            found = true;
            break;
        }
    }
    if (!found) { write_str(argv[0]); write_str(": command not found\n"); }
}
```

`read_line` 才是交互体验的所在,关键是它**逐字符**读、**当场**回显、**当场**处理退格:

```cpp
size_t read_line(char* buf, size_t cap) {
    size_t pos = 0;
    while (pos < cap - 1) {
        char c = 0;
        int64_t n = sys_read(0, &c, 1);       // 一次只取一个字符
        if (n <= 0) continue;
        if (c == '\n') { write_buf("\n", 1); break; }   // 换行: 回显并收尾
        if (c == 0x7F || c == '\b') {                    // 退格
            if (pos > 0) { --pos; write_buf("\b \b", 3); }
            continue;
        }
        write_buf(&c, 1);                    // 普通字符: 回显并存
        buf[pos++] = c;
    }
    buf[pos] = '\0';                         // 换行不存进 buf
    return pos;
}
```

几个「为什么这样写」值得点出来。其一,退格发的是三字节 `\b \b`(退格、空格、退格),不是单个 `\b`。因为终端光标只 `'\b'` 会左移一格但**不擦**内容,得用空格把那个字盖掉、再退一格把光标停回原位——这是 VT100 时代留下的擦除套路,我们的 Console 在 [console.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/video/console.cpp) 的 `putc` 里把 `'\b'` 实现成「列号减一」,所以这串三字节恰好完成「左移、用空格覆盖、再左移」。其二,换行符 `'\n'` **只回显、不存进 `buf`**——`break` 在 `buf[pos++]` 之前发生,所以行缓冲里是个干净的、不含换行的字符串,后面 `tokenize` 不用特判结尾。其三,`sys_read` 返回 `<=0` 时 `continue` 而非报错,因为 024 的 `sys_read` 在键盘空且 spin-wait 超时后会返回 0,这时不该让 shell 退出,该再等一轮。

### tokenize:就地把空格变 NUL

[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/programs/shell/main.cpp) 是 freestanding 环境下最省事的分词法——不分配内存,直接在原缓冲区上动刀:

```cpp
size_t tokenize(char* line, char** argv, size_t max_tokens) {
    size_t argc = 0;
    while (*line != '\0' && argc < max_tokens) {
        while (*line == ' ' || *line == '\t') ++line;     // 跳过前导空白
        if (*line == '\0') break;
        argv[argc++] = line;                              // 记下这个词的起点
        while (*line != '\0' && *line != ' ' && *line != '\t') ++line;  // 走到词尾
        if (*line != '\0') *line++ = '\0';                // 把分隔符就地改成 NUL
    }
    return argc;
}
```

为什么能这么写?因为 `line` 是 `shell_main` 栈上的 `char line[MAX_LINE]`,`read_line` 往里填了字符、末尾加了 `'\0'`,它是一块可写的、连续的、以 NUL 收尾的内存。`tokenize` 把词与词之间的空格/Tab 直接覆盖成 `'\0'`,于是原本一整行 `"echo hello"` 就变成了两个以 `'\0'` 分隔的 C 字符串 `"echo"` 和 `"hello"`,`argv[0]`、`argv[1]` 分别指向它们——`strcmp` 和各 `cmd_*` handler 拿到的就是标准的 C 字符串。没有 `malloc`,没有拷贝,没有内存要还。`max_tokens` 这个参数是保险:输入再长,`argv[]` 也只填到 `MAX_TOKENS=16` 个就停,不会写越界。

### builtin_cmds[]:哨兵结尾的分发表

命令表是个编译期常量数组,以 `{nullptr,nullptr}` 收尾:

```cpp
constexpr CmdEntry builtin_cmds[] = {
    {"echo",  cmd_echo},
    {"help",  cmd_help},
    {"clear", cmd_clear},
    {nullptr, nullptr},      // 哨兵: 遍历到 name==nullptr 即停
};
```

派发时遍历到哨兵就停,这是 C 风格变长表的标准手法,省得单独维护一个 `count`。`CmdEntry` 的结构在 [shell.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/programs/shell/shell.hpp) 里定义——`{const char* name; void (*handler)(int,char**)}`,handler 统一签名为 `(argc, argv)`,哪怕像 `clear` 这样不需要参数的命令,也照收 `argc/argv` 然后忽略。这种「统一签名 + 各自忽略不需要的参数」的做法,让加一条新命令的成本极低:写个 `cmd_xxx.cpp`、在 `shell.hpp` 声明、在表里加一行。024 只填了这三条,扩展点已经留好了。

三个 handler 的实现都短得可怜:`cmd_echo` 把 `argv[1..]` 用单空格连起来加个 `\n`、`cmd_help` 打一段固定的命令清单、`cmd_clear` 发 7 字节的 `\033[2J\033[H`。这里特别说明 `cmd_clear` 为什么发的是**两个**转义:`\033[2J` 是「擦除整个屏幕」、`\033[H` 是「光标归位到 (1,1)」——只擦不归位,光标会停在原处,新输出接在后面显得没清干净;两个一起发,屏才真清干净。这 7 个字节能不能起作用,完全取决于内核 Console 会不会吃这串 ANSI CSI,那就是下面 Console 那块的事。

### sys_read:spin-wait 取键盘、一次给一行

内核侧 [sys_read.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/sys_read.cpp) 是个非常克制的处理器。先两道守卫:fd 只认 0(只有 stdin),用户缓冲地址必须低于 `0x800000000000`(不能让用户态借 read 把数据写进内核高半区):

```cpp
if (buf_virt >= USER_ADDR_MAX) return -1;   // USER_ADDR_MAX = 0x800000000000
if (fd != 0) return -1;
```

然后是核心循环。它从 `Keyboard::poll` 取一个 `KeyEvent`,只接受「按下且 ascii 非零」的事件(松开事件、功能键之类统统丢),把 `\r` 转成 `\n`,遇 `\n` 就停:

```cpp
while (read_bytes < count) {
    KeyEvent ev;
    if (!Keyboard::poll(ev)) {
        if (read_bytes > 0) break;                  // 已经有数据, 立即返回
        // 还一个字都没有: spin-wait 等第一个字符
        bool got_key = false;
        for (uint32_t i = 0; i < SPIN_WAIT_ITERS; i++) {   // SPIN_WAIT_ITERS = 1'000'000
            __asm__ volatile("pause");
            if (Keyboard::poll(ev)) { got_key = true; break; }
        }
        if (!got_key) break;                        // 超时仍无输入, 返回 0
    }
    if (!ev.pressed || ev.ascii == 0) continue;     // 只要按下且可打印
    char ch = (ev.ascii == '\r') ? '\n' : ev.ascii; // \r → \n, 给 shell 一个统一的行尾
    buf[read_bytes++] = ch;
    if (ch == '\n') break;                          // 一行结束, 停在这里
}
return static_cast<int64_t>(read_bytes);
```

两个设计选择得讲清。其一,**为什么 spin-wait 而不阻塞**?因为 024 还是单任务——`launch_first_user` 只起了 shell 一个进程,shell 不返回。没有别的任务可调度,「阻塞当前任务、等键盘中断把它唤醒」这套机制(需要调度器、需要把键盘 IRQ 接到阻塞队列)在本章根本不存在。所以最朴素的做法就是 `pause` 死等到字符出现为止,`SPIN_WAIT_ITERS=1'000'000` 这个上限只是个保险——万一键盘真的一直没输入,转完这一百万圈就返回 0,shell 的 `read_line` 见到 `n<=0` 就 `continue` 再来一轮,不至于把 CPU 永久焊死在内核里。其二,**为什么 `\r` 要转 `\n`**?PS/2 键盘按回车产生的扫描码,经过键盘驱动解码后,`KeyEvent.ascii` 里填的是 `'\r'`(回车);但 shell、C 字符串、`tokenize` 全都拿 `'\n'` 当行尾。在 `sys_read` 这一层统一转掉,shell 那侧就不用关心键盘到底吐的是 `\r` 还是 `\n`。这一层「内核把硬件的怪癖抹平、给用户态一个干净语义」的分工,是 read 能用起来的关键。

### GDT 重排:为什么 0x10/0x18/0x33/0x2B/0x38,以及那个 TLS 占位

[gdt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/gdt.cpp) 的 `init()` 现在填 9 项。其中 idx 1(选择子 0x08)是个**占位**:

```cpp
entries_[0] = null_entry();
entries_[1] = null_entry();   // idx 1 (0x08): 占位 (TLS 预留, 现在空着)
entries_[2] = segment_entry(/* Kernel Code,  Ring0, Exec|RW, LongMode */);  // 0x10
entries_[3] = segment_entry(/* Kernel Data,  Ring0, RW, Size32 */);         // 0x18
entries_[4] = segment_entry(/* User32 Code,  Ring3, Exec|RW, Size32 */);    // 0x20
entries_[5] = segment_entry(/* User Data,    Ring3, RW, Size32 */);         // 0x28
entries_[6] = segment_entry(/* User64 Code,  Ring3, Exec|RW, LongMode */);  // 0x30
entries_[7] = tss_low_entry(...);   entries_[8] = tss_high_entry(...);      // TSS, 0x38
```

为什么 idx 1 留个空的占位?这是在**对齐 Linux 的 GDT 布局**:Linux 在 0x08 那个位置放的是 per-CPU 的 TLS(线程局部存储)段。024 还没实现 TLS,但把位置占住,后面要加就不用再动一遍选择子编号——选择子一旦写进 STAR/GDT 测试/用户态链接脚本,改一处就得连带着改一堆,所以现在就按目标布局摆好。`kEntryCount=9`、`GDT_USER_CODE=0x33`、`GDT_USER_DATA=0x2B` 这些常量随之在 [gdt.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/gdt.hpp) 定下来。注意 `0x33 = 0x30 | 3`、`0x2B = 0x28 | 3`——用户态选择子本来就**自带 RPL=3**,这一点马上就是 024-01 的关键。

真正的新东西是 `GDT_SYSRET_BASE = 0x23`。它在 [syscall.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/syscall.cpp) 里被拼进 STAR:

```cpp
uint64_t star_val = (static_cast<uint64_t>(GDT_SYSRET_BASE) << 48)   // STAR[63:48] = 0x23
                  | (static_cast<uint64_t>(GDT_KERNEL_CODE) << 32);  // STAR[47:32] = 0x10
write_msr(MSR_STAR, star_val);
```

`0x23` 这个值不是随便挑的。SYSRETQ 出口会读 `STAR[63:48]` 做两次加法得到目标 CS/SS:`CS = base+16`、`SS = base+8`(Intel SDM Vol.3A p.184 原文:`Stack segment — IA32_STAR[63:48] + 8`)。当 `base=0x23` 时,`0x23+16 = 0x33`、`0x23+8 = 0x2B`——正好是两个自带 RPL=3 的用户选择子。SYSCALL 入口读的是 `STAR[47:32]=0x10`,算出 CS=`0x10`、SS=`0x10+8=0x18`,对应内核态,不受影响。这一改让 SYSRETQ 的算术结果**天然带 RPL=3**,不再依赖「CPU 算完之后会不会再 OR 一个 3」——而那正是 024 崩溃的根子。

### Console 的 ANSI CSI 状态机:吃 ESC[2J / ESC[H

为了让 `cmd_clear` 发的那 7 字节 `\033[console.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/video/console.cpp)):

```cpp
switch (ansi_state_) {
case Normal:
    if (c == '\x1B') { ansi_state_ = Esc; return; }   // ESC: 进入 Esc 态
    break;                                            // 否则按正常字符往下走
case Esc:
    if (c == '[') { ansi_state_ = Bracket; ansi_pos_ = 0; return; }  // ESC[: 进参数收集
    ansi_state_ = Normal; break;                      // 不是 CSI, 丢弃 ESC, 回 Normal
case Bracket:
    if (ansi_pos_ < sizeof(ansi_params_)-1 &&
        ((c >= 0x30 && c <= 0x3F) || (c >= 0x20 && c <= 0x2F))) {
        ansi_params_[ansi_pos_++] = c; return;        // 参数字节 0x30-0x3F 或中间字节 0x20-0x2F
    }
    if (c >= 0x40 && c <= 0x7E) {                      // 终止字节 0x40-0x7E: 派发
        ansi_params_[ansi_pos_] = '\0';
        handle_ansi_csi(c);
        ansi_state_ = Normal;
        return;
    }
    ansi_state_ = Normal; return;                      // 畸形序列, 复位
}
```

那几个字节区间不是拍脑袋。ANSI CSI 序列的结构是 `ESC[` 后跟一串**参数字节**(`0x30`–`0x3F`,数字和分号)、可选的**中间字节**(`0x20`–`0x2F`)、最后以一个**终止字节**(`0x40`–`0x7E`,决定干什么)收尾。状态机按这三段区间逐字节归类,遇到终止字节就调 `handle_ansi_csi`。`handle_ansi_csi` 目前只认两条:`'J'` 配参数 `2` 调 `clear()`(全屏擦除),`'H'` 把光标 `col_=row_=0`(归位)。其余 CSI 序列(颜色、光标移动之类)一律忽略——`default` 分支静默丢弃。这是「只实现 `cmd_clear` 用得到的那两条、其余安全忽略」的最小可用集。

## 调试现场

这一章的两颗雷都是「看起来 shell 能跑、实际埋着炸」的典型,而且都炸在 SYSCALL/SYSRET 这条用户态往返的路径上。

### 案例一:shell 起来打完 prompt,下一个 PIT tick 就 #GP(错误码 0x28)

**症状**:shell 在 Ring 3 成功起来,串口打出 `Cinux shell - type 'help' for commands` 和 `cinux> `,之后第一次 PIT 时钟中断(IRQ0)触发 IRETQ 时内核崩成 `#GP`,错误码 `0x28`。崩溃点经 `addr2line` 定位是 `irq0_stub` 的那条 `iretq`。

**根因**:`0x28` 这个错误码本身就是线索——它指向 GDT 第 5 项(User Data 段)、RPL=0。在 `pit_irq0_handler` 的 C handler 里打印中断帧的 `frame->cs`/`frame->ss`,看到进入中断时用户态的 `SS=0x0028`、而正确值应该是 `0x002B`(RPL=3);CS 倒是 `0x0033`,对的。也就是说,SYSRETQ 把用户态带回来时,给 CS 算对了、给 SS 漏了 RPL=3。SS 的 DPL/RPL 是 0、当前 CPL 却是 3,下一次 IRETQ 想把 SS 加载成 `0x28` 时,特权检查不过,炸 `#GP(0x28)`。

这里有个**必须说准**的点。Intel SDM Vol.2B 第 717 页 SYSRET 伪代码明写:返回路径上 `SS.Selector := (IA32_STAR[63:48]+8) OR 3; (* RPL forced to 3 *)`——规范是会强制 RPL=3 的。所以根因**不是**「SYSRET 不设 RPL」,而是我们这台 QEMU/TCG 在 SYSRETQ 路径上**没执行这个 `OR 3`**(对 CS 执行了,所以 CS 对;对 SS 没执行,所以 SS 错)。这是模拟器行为,不是 CPU 规范行为。

**修复**:与其赌 QEMU 会不会老实地 OR 3,不如把 RPL=3 直接编码进 STAR 基值。GDT 重排进行到一半、kernel CS 已挪到 `0x10` 时,SYSRET base 一度停在中间态 `0x20`(调试现场里 STAR 回读 `0x00200010` 就是这一刻);024-01 再把它从 `0x20` 定到 `0x23`,`usermode.S` 里写 STAR 的立即数同步成 `$0x23`。于是 `+8=0x2B`、`+16=0x33`,两个结果本身就带 RPL=3,无论模拟器要不要再 OR 一次,选择子都对。注意这个 `0x20` 只是重排途中的过渡值——023 tag 落地的 STAR 基值还是 `0x08`(那版内核 CS 在 `0x08`),并不是 `0x20`。GDT 描述符一行都不用动,因为 `0x33` 指向 idx6、`0x2B` 指向 idx5,索引没变。

**防复发**:SYSRETQ 出口的 SS.RPL 不要依赖硬件/模拟器事后补,基值自带 RPL 最稳。这和主流 x86-64 内核(如 Linux)的 STAR 写法是同一思路。诊断时,在 ISR 的 C handler 打 `frame->cs`/`frame->ss` 是区分内核/用户态中断、抓 SS 异常最快的一招;`#GP` 错误码在 IRETQ 场景下就是 CPU 试图加载的那个 selector,对着 GDT 布局一眼就能定位是哪个描述符。

### 案例二:回显正常,但 `echo hello` 打不出 hello

**症状**:shell 起来正常、提示符正常、敲键盘每个字符都能正确回显——说明 `sys_read` + `sys_write` 基本通路是通的。可是一按回车,`echo hello` 不出 `hello`,`clear` 也不清屏。所有命令集体失声。

**根因**:所有命令都失效,几乎一定不是某个命令的 bug,而是底层共性出了问题。在内核各层(syscall_dispatch / sys_read / sys_write)和 shell 主循环都加 debug 打印后,抓到一行决定性的 trace:用户明明敲了 `echo hello`(10 个字符),但 shell 拿到的 `line` 是空串、`len=1`。反汇编 `user_shell` ELF,看到编译器把 `read_line` 内联进了 `shell_main`,并把循环里的写入位置 `pos` 分配到了 RBX——核心就是这几行:

```asm
call   sys_read            ; 读一个字符
...
call   sys_write           ; 回显
lea    rdx, [rbx+0x1]      ; rdx = pos + 1
mov    BYTE PTR [rsp+rbx+0x90], al   ; line[pos] = c   ← 写到 line[rbx]
mov    rbx, rdx            ; pos = pos + 1
```

也就是说 `pos` 这个关键变量活在 RBX 里。可是每次 `sys_read`/`sys_write` 都要走 SYSCALL 进内核,而当时 [syscall.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/syscall.S) 的出口段有这么一句:

```asm
movq %rax, %rbx        # ← 用 RBX 暂存返回值!
...
addq $96, %rsp         # 释放整个 trap frame
...
movq %rbx, %rax        # 恢复返回值
```

它拿 RBX 当返回值的临时落脚点。可 RBX 是 **System V AMD64 ABI 的 callee-saved 寄存器**(和 `rbp`、`r12`–`r15` 一组),约定要求它跨函数调用保持不变。SYSCALL/SYSRET 这趟往返,从用户态角度看就是一次函数调用——返回时 RBX 必须和进去时一模一样。而 SYSCALL 指令**只**自动保存 RCX(存返回 RIP)和 R11(存 RFLAGS)两个寄存器,其余全靠软件。旧代码把返回值塞进 RBX,正好覆盖了用户的 `pos`;trap frame 里虽然压着原始 RBX(在 `rsp+80`),但出口段没去取回它,`addq $96,%rsp` 一抬手就把 frame 整个释放了,原始 RBX 彻底丢失。结果每个字符都写进 `line[1]`(RBX 被盖成返回值 1),回车时 `line[1]` 又被写成 `'\0'`,整行就成了空串——`tokenize` 切不出 `echo`,`strcmp` 自然全 miss。

**修复**:返回值换一个不会被用户态当成「承诺不变」的地方存。正好 per-CPU 的 GS scratch 页还有个空闲槽 `gs:16`(`gs:0` 存内核 RSP、`gs:8` 存用户 RSP,`gs:16` 没人用)。出口段改成:

```asm
movq %rax, %gs:16        # 返回值存进 GS scratch, 不碰 RBX
movq 0(%rsp), %rax
movq %rax, %gs:8
movq 8(%rsp), %rcx
movq 16(%rsp), %r11
movq 80(%rsp), %rbx      # ◄ 从 trap frame rsp+80 恢复用户 RBX
addq $96, %rsp
movq %gs:8, %rsp
movq %gs:16, %rax        # 取回返回值
swapgs
sysretq
```

两处改动:返回值走 `gs:16`,并在销毁 frame 之前从 `rsp+80` 把用户的 RBX 还回去。顺带一提,用户态那个 `printf` 就是这次排错逼出来的——当初想格式化数字打 debug,手写内联代码踩了空指针 `#PF`,索性把一个完整的 `printf`(支持 `%d %x %p %l %ll`)写进了 `user_libc`。

**防复发**:SYSCALL 入口/出口等同于一次函数调用,callee-saved 寄存器(`rbx/rbp/r12–r15`)必须原样归还——哪怕内核内部需要暂存,也得先存好、再恢复,绝不能拿它们当 scratch。SYSCALL 只自动管 RCX/R11,这条要刻进脑子里。怀疑寄存器被 clobber 时,反汇编用户态和内核态的入口/出口是唯一能一锤定音的手段:编译器把哪个变量分配到哪个寄存器,只有看了汇编才知道。

## 验证

shell 的纯逻辑(字符串工具、tokenizer、命令分发表、`cmd_echo`/`cmd_help`/`cmd_clear` 的行为、`read_line` 的退格与换行处理)在 host 上镜像着测。[test_shell.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_shell.cpp) 把 `cinux::user::strlen/strcmp/memset/memcpy/memcmp` 的边界、`tokenize`(单词/多词/首尾空白/Tab/`max_tokens` 截断/空串/纯空白)、`CmdEntry` 哨兵表的查找与未命中、三个命令的输出、`read_line` 的退格行为在 host 侧重写了一份(不链内核代码,`sys_write`/`sys_read` 用 mock,`CINUX_HOST_TEST` 门控,优化级随 CMake build type 走、默认 Debug 即 `-O0`):

```bash
ctest --test-dir build -R shell --output-on-failure
```

真正的内核基础设施(`sys_read`/`sys_write` 的 fd 与地址守卫、`SyscallNr` 常量、GDT 重排后的选择子常量、STAR 的 `0x23`/`0x10` 回读)只能在 QEMU 里验。[test_shell.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_shell.cpp) 在机内重写 tokenizer/字符串/mem 工具、直接调 `sys_write` 验守卫、断言 `SYS_read=0 / SYS_write=1 / SYS_exit=60`;而 [test_gdt_idt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_gdt_idt.cpp) 和 [test_syscall.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_syscall.cpp)、[test_usermode.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_usermode.cpp) 把「GDT 重排 + STAR 改 0x23」钉死——`GDT_KERNEL_CODE=0x10`/`GDT_USER_CODE=0x33`/`GDT_USER_DATA=0x2B`/`GDT_TSS=0x38`、`STAR[47:32]=0x10`、`STAR[63:48]=GDT_SYSRET_BASE=0x23` 三处回读。机内 test 节名是 `Shell Tests (024)`:

```bash
cmake --build build --target run-big-kernel-test
```

机内末尾打 `ALL TESTS PASSED` 就说明这套 shell 基础设施在真硬件语义下成立。要说明一句:真用户态 shell 跑在 Ring 3,机内测无法直接调它,只能验它**依赖的内核基础设施**——这是 host 单测(QEMU 之外的纯逻辑)和机内测(QEMU 里的内核侧)分工的原因。

最后是**生产现象**本身:直接跑大内核,串口应该看到 `Cinux shell - type 'help' for commands` 和 `cinux> ` 提示符;敲 `echo hello` 回车,下一行出现 `hello`;敲 `help` 回车,列出 `echo/help/clear` 三条;敲 `clear` 回车,屏幕擦净、光标归位;退格能删字。这三条命令各自行为正确、退格可用,且 SYSRETQ 出口的 CS=0x33/SS=0x2B、syscall 不再破坏用户 RBX——两颗雷都拆干净,就算 024 到位。

## 下一站

到这里,内核第一次有了一个**常驻 Ring 3、能和用户来回交互**的程序。shell 读键盘、切参数、查表派发,`sys_read` 把键盘字符一行行递上来,GDT 重排让 SYSRETQ 稳稳地穿越特权级,Console 会吃最基础的 ANSI CSI。

但你会立刻摸到 024 的天花板:这个 shell **只在内存里转**。它没有 `cat`、没有 `ls`、没有任何能碰「持久存储」的命令——因为根本没有持久存储,数据一断电就没了。shell 想读个文件,既没有文件系统,也没有哪块磁盘被接上。下一站(025)就补这块:把 AHCI/PCI 驱动接进来,让内核能和(模拟的)SATA 磁盘说上话,给数据落盘铺好物理基础。有了能读写的块设备,后面才会真正长出文件系统、以及 `cat`/`ls` 这些文件系统命令。024 的 shell 是那一切的入口——先把「人机对话」这条路走通,接下来才有意义去对话「给我看磁盘上的那个文件」。

---

### 参考

- **Intel SDM Vol.2B,SYSRET 伪代码(p.717)**(本地 `document/reference/intel/SDM-Vol2B-Instruction-Reference-M-U.pdf`):SYSRET 返回路径 `CS.Selector := IA32_STAR[63:48]+16`、`SS.Selector := (IA32_STAR[63:48]+8) OR 3; (* RPL forced to 3 *)`——用 `pdf-reader` search_pdf 核实(p.717,match `p717-match-1/3`,2026-06-21)。这是 024-01 的「对照组」:规范明确会强制 SS.RPL=3,崩溃根因是 QEMU/TCG 在 SYSRETQ 路径上没执行这一步(对 CS 执行了),故 CS 对、SS 错。
- **Intel SDM Vol.3A,SYSCALL/SYSRET 段选择子(p.184)**(本地 `document/reference/intel/SDM-Vol3A-System-Programming-Guide-Part1.pdf`):`Stack segment — IA32_STAR[63:48] + 8`(核实 p.184,match `p184-match-1`,2026-06-21)。配合 `GDT_SYSRET_BASE=0x23` 得到 SYSRETQ 目标 SS=0x2B、CS=0x33,与 `gdt.hpp` 常量一致。
- **System V AMD64 ABI**([x86-psABIs/x86-64-ABI](https://gitlab.com/x86-psABIs/x86-64-ABI),Sec. "Register Usage"):callee-saved = `rbx/rbp/r12–r15`——024-02 修复(返回值改存 `gs:16`、从 `rsp+80` 恢复用户 RBX)的依据。该清单在 019 章已按 live 核过并沿用;本次本机联网未稳定命中该页正文,标 `assumed`,TODO(reference):联网恢复后补 page anchor。
- **SYSCALL 只自动保存 RCX/R11**:Intel SDM Vol.2B,SYSCALL 伪代码(`RCX ← RIP`、`R11 ← RFLAGS`,SDM 同卷)——024-02「其余寄存器全靠软件保存」的依据。TODO(reference):本次未单独 search_pdf 命中 SYSCALL 伪代码页号,联网/预算允许时补精确页号。
- 023 章 · [让用户态会说话:SYSCALL/SYSRET 系统调用](023-syscall.md):`syscall_entry` 的 trap frame 布局与「SYSCALL 不压栈、只存 RCX/R11」的由来——本章 024-02 修复直接踩在 023 搭好的 frame(`rsp+80` = 用户 RBX)上。
- 本 tag 源码:[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/programs/shell/main.cpp) / [shell.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/programs/shell/shell.hpp) / [cmd_echo.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/programs/shell/cmd_echo.cpp) / [cmd_clear.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/programs/shell/cmd_clear.cpp) / [cmd_help.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/user/programs/shell/cmd_help.cpp)、[sys_read.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/syscall/sys_read.cpp)、[gdt.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/gdt.hpp) / [gdt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/gdt.cpp)、[syscall.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/syscall.cpp) / [syscall.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/syscall.S) / [usermode.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/usermode.S)、[console.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/video/console.cpp);测试 [test_shell.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_shell.cpp)(host 镜像)、[test_shell.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_shell.cpp)(QEMU 机内,节 `Shell Tests (024)`)。
