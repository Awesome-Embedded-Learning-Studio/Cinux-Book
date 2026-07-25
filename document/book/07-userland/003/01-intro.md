---
title: 01 · 给内核一个能对话的用户态:shell
---

# 给内核一个能对话的用户态:shell

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
