---
title: 02 · 代码路线:REPL、tokenize、sys_read、GDT 重排与 ANSI CSI
---

# 代码路线:REPL、tokenize、sys_read、GDT 重排与 ANSI CSI

## shell_main 与 read_line:一个逐字符的 REPL

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

## tokenize:就地把空格变 NUL

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

## builtin_cmds[]:哨兵结尾的分发表

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

## sys_read:spin-wait 取键盘、一次给一行

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

## GDT 重排:为什么 0x10/0x18/0x33/0x2B/0x38,以及那个 TLS 占位

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

## Console 的 ANSI CSI 状态机:吃 ESC[2J / ESC[H

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
