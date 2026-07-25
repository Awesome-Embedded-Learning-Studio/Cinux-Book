---
title: 01 · musl 静态移植:对齐 Linux ABI、铺初始栈,以及一个被 SMAP 拦下的潜伏 bug
---

# musl 静态移植:对齐 Linux ABI、铺初始栈,以及一个被 SMAP 拦下的潜伏 bug

> 之前用户态跑的是 Cinux 自己那个极简 libc(手写的 `syscall.h` 那几个壳)。这一章换一套做法:让内核能跑**用真 musl libc 编译的静态程序**——musl 是 Linux 世界里常用的轻量 libc,用它编译的程序(比如 `hello world`)静态链接一个 `libc.a` 进来,扔到内核上就能跑。要做到这件事,内核得**把自己对齐到 Linux 的 ABI**:syscall 号得和 Linux x86_64 一致、返回值得是负 errno、给程序铺的初始栈得带 musl 启动要读的辅助向量(auxv)。这一章把这三件事做了,然后跑 musl 的 hello——结果 hello 一跑,挖出一个在内核里**潜伏了很久的真 bug**,而这个 bug 之所以一直没被发现,正好和上一卷(056)讲的 SMAP / WSL2 那条边界有关。验证口径:punchline 是 musl 编译的 `hello` 真在内核里跑起来、打出 `Hello from musl`、干净退出。内核侧的 ABI + 初始栈靠测试验证;musl 程序的端到端跑通靠一个专门的 ring3 测试。
>
> 一条诚实的边界先说在前头:这一步的 musl 程序是**静态链接**的(把 musl 的 `libc.a` 整个链进可执行文件),不是动态链接(没有 `ldso`、没有 `PT_INTERP`)。动态链接是后面的事。而且 musl 的工具链(`build-musl.sh` 编出 sysroot)得先在宿主机上编一次,这一章讲的是内核侧怎么准备好接住 musl 程序,不是手把手教编 musl。

## 这章咱们要点亮什么

1. **Linux ABI 对齐**:syscall 号、负 errno 返回约定、结构体布局(stat/sigaction)——musl 直接发原始 syscall,内核必须按 Linux 规矩应答。
2. **一个撞号的真 bug**:`SYS_chdir` 和 `SYS_brk` 都被定义成 12,`cd` 命令发的 syscall 12 实际命中了 `sys_brk`。
3. **初始栈 + auxv**:musl 启动时从栈上读辅助向量(`AT_PHDR`/`AT_RANDOM`/`AT_UID`...),内核的 `execve` 原来根本不铺栈内容。
4. **那个被 SMAP 拦下的潜伏 bug**:跑 musl hello 时首次进 ring3 就 #DF(双重错误),根因是 `jump_to_usermode` 切到用户栈之后、在内核态写了一下用户内存设 RFLAGS——SMAP 一开就 #PF、#PF 推栈又失败变 #DF。它在 WSL2 上一直不发作,正是因为 WSL2 不透传 SMAP(056 讲过的那条边界)。

## Linux ABI:把 syscall 号和返回约定对齐

musl 不走 Cinux 自己那个 `user/libc/syscall.h`,它有自己一套预编译的 syscall 桩(`musl-gcc` 编程序时就链进去了),直接 `syscall` 指令发**Linux 标准号**。所以内核这张 syscall 号表,必须和 Linux x86_64 一致——musl 发 `SYS_write=1`,内核的号 1 就得是 write。

对齐的时候全表核对了一遍,**几乎全对,除了一处真 bug**:`SYS_chdir` 和 `SYS_brk` 都被定义成了 12:

```cpp
SYS_brk   = 12,   // set program break / heap end (F2-M3)
...
SYS_chdir = 80,   // change working directory (was wrongly 12, collided with brk)
```

（`syscall_nums.hpp:35`/`:68`。）注册的时候 `chdir` 在前、`brk` 在后,`brk` 把 slot 12 **覆盖**了——`chdir` 实际不可达,shell 的 `cd` 发 syscall 12,命中的是 `sys_brk`(坏的)。Linux 里 chdir 是 80、brk 是 12,改成 `SYS_chdir = 80` 就对了。这是个潜伏的真 bug:`cd` 一直是坏的,只是没人盯着它「为什么 cd 不生效」深究。

> **号表的单一事实源。** 用户侧那个 `user/libc/syscall.cpp` 不硬编码号,而是 `#include "kernel/syscall/syscall_nums.hpp"` 直接用 `SyscallNr::SYS_chdir`。改 enum 一处,内核和用户壳自动同步(只要全量重编)。这样不会再出现「内核改了号、用户壳没跟上」的漂移。

返回约定也得收齐:Linux 的 syscall 出错返**负 errno**(`-EPERM` 这种),不是裸 `-1`。Cinux 大部分已经是这风格(`sys_open` 返 `-to_errno(...)`),只有几个老 syscall 残留裸 `-1`(比如 `getcwd` 坏地址返 `-1`),这一步按场景补成正确的负 errno(`-kEfault`/`-kEinval`/`-kEsrch`)。结构体布局(stat、sigaction)也得对齐 Linux UAPI——musl 按 Linux 的 `struct stat` 字段顺序读,内核填的得一样,否则字段错位。这些是「读 musl 源码 + Linux UAPI,不猜」逐个对的。

## 初始栈:musl 启动要读的 auxv

这是 musl 能不能起得来的关键。musl 的启动链(`_start` → `_start_c` → `__libc_start_main` → `__init_libc`)一进来就从用户栈上读**辅助向量(auxv)**:它要 `AT_PHDR`/`AT_PHNUM`/`AT_PHENT` 找程序头(定位 TLS),`AT_PAGESZ`(断言非 0),`AT_RANDOM`(给栈 canary 取种子),`AT_UID/EUID/GID/EGID` + `AT_SECURE`,等等。原 Cinux 的 `execve`/`launch_user_program` **完全不铺栈内容**——argv/envp 被忽略,RSP 指向一个 demand-fault 出来的零页。musl 一读 `argc=[rsp]=0`、auxv 全无,直接起不来。

所以得照 Linux 的规矩铺初始栈。布局(低地址→高地址)是:

```
[ argc | argv[] | NULL | envp[] | NULL | auxv.. | AT_NULL | strings.. | pad ]
```

（`initial_stack.hpp:9`。）这是 Linux x86_64 进程入口的标准栈帧。实现成一个纯函数 helper `build_initial_stack`,把这块内容**右对齐**铺进 caller 给的 buffer(buf 末尾 ↔ 用户栈顶 `stack_top`),这样 caller 能把栈顶页的 direct-map 内核虚址直接当 buffer 传——零拷贝。几个硬约束:

- argv/envp 指针、`AT_RANDOM`/`AT_EXECFN` 的值都是**绝对用户虚址**(从 `stack_top` 反推),不是相对偏移。
- 入口 RSP(`= stack_top - size`)**16 字节对齐**(SysV ABI 进程入口约定,musl 的 SSE 指令指望这个)。
- helper 是 header-only、kernel 和 host 单测双编译、自带 `strlen`/`memcpy`(freestanding 内核没有 `<cstring>`)。

```cpp
constexpr uint64_t AT_PHDR    = 3;
constexpr uint64_t AT_PHENT   = 4;
constexpr uint64_t AT_PHNUM   = 5;
constexpr uint64_t AT_PAGESZ  = 6;
constexpr uint64_t AT_RANDOM  = 25;   // SSP canary 种子
constexpr uint64_t AT_UID     = 11;
...
```

（`initial_stack.hpp:37` 起,`AT_*` 那一列。)这个 helper 有 host 单测直接验栈布局(`test/unit/test_initial_stack.cpp`):走 argc/argv/envp/auxv 到 `AT_NULL`,断言 16 字节对齐 + 关键 AT_ 键都在——这就是内核侧 musl 就绪的客观证据。

## 新 syscall:musl 要的那几个

musl 启动链里会用几个 Cinux 原来没有(或不全)的 syscall,这一步补上:`sys_open`(`open`/`openat`,musl 加载文件)、`sys_stat`(`newfstatat`,musl 查文件元信息)、`sys_set_tid_address`(musl 设 cleartid 地址,跟 clone/TLS 那套机制配套)。还有 `arch_prctl`(`ARCH_SET_FS` 设 TLS 的 `fs_base`,musl 的 `__init_tp` 要)之类。这些都是 musl 启动到某一步会发的 syscall,内核得有 handler 应答,否则 musl 拿到 `-ENOSYS` 就走不下去。

## 跑 musl hello:一个被 SMAP 拦下的潜伏 bug

前面三件事(ABI、初始栈、syscall)都对齐了,该跑 musl hello 了。hello 是 `tools/musl/hello.c` 用 `musl-gcc` 编的静态小程序,链进 musl 的 `libc.a`。在内核测试里用一个 ring3 smoke 把它端到端跑一遍——结果**第一次进 ring3 就 #DF(Double Fault)**。

定位这个 #DF 是这一章最值得记的。用 `addr2line`(拿 `LSTAR=syscall_entry` 当锚算出加载基址)反推 RIP,落在 `jump_to_usermode`——就是内核切到用户态的那个函数。诊断打印确认 `gs:0`(per-CPU)、LSTAR 都对,所以 #DF 发生在 `jump_to_usermode` 执行期间。看那段汇编:

```asm
jump_to_usermode:
    ...
    movq %rsi, %rsp        # 先切到用户栈
    ...
    movq $0x202, %r11      # RFLAGS(IF|bit1),SYSRET 恢复
```

（`usermode.S:96`。)问题在**修复前**的那一版:它切到用户栈(`mov %rsi,%rsp`,RSP 已经指向用户内存)之后,用的是 `pushq $0x202; popq %r11` 来设 RFLAGS。`pushq` 往**当前 RSP 指的内存**写——而此时 RSP 是用户栈,**这是内核态写用户内存**。SMAP(056 开的那个)一开,内核写用户内存 → #PF;而此时 RSP 指向用户栈,#PF 想往内核栈推错误码却推不进去(用户栈不让内核写)→ 推栈失败 → 升级成 #DF。修法就是上面那行 `movq $0x202, %r11`:用**立即数加载**设 RFLAGS,根本不碰内存,等价且安全。

> **这个 bug 为什么潜伏到现在?** 因为上一卷(056)讲过的那条边界:本机是 WSL2 嵌套 KVM,它不透传 CPUID.07H:EBX 的 SMAP 位,所以 `enable_smep_smap()` 的 CPUID gate **没开 CR4.SMAP**——SMAP 在开发机上压根没生效,`pushq` 写用户内存没被拦,程序照跑。换真机、或完整 KVM、或 TCG(SMAP 透传的环境),这个 bug 会让生产环境的 `/bin/sh` 一启动就 #DF。它是个**和 SMAP 强相关的、生产相关的真 bug**,只是被 WSL2 的环境限制挡住了,直到这一步跑 musl hello 的 ring3 测试才第一次暴露。这是「环境限制掩盖了真 bug」的典型——开发机上绿,不代表真机上对。

修完之后,musl hello 的完整启动链在 Cinux ring3 跑通:

```
[EXECVE] loaded /hello entry=0x40103B pid=10
Hello from musl on CinuxOS!         ← musl write(1,...) → SYS_write → kprintf
[SYSCALL] sys_exit(0) from tid=139  ← musl exit_group(0) 干净退出
```

整条 musl 启动链(`_start` → `__libc_start_main` → `__init_libc` 读 auxv → `__init_tls`/`__init_tp` 设 TLS → 读栈 canary → `main` → `write` → `exit_group`)全端到端过了。这一步前面铺的 ABI / 初始栈 / syscall 工作全部经住了真 musl 程序的检验。

> **投资 ring3 测试的回报。** 跑通这个 hello 不是白跑——为了在内核测试里把一个 ring3 程序端到端跑起来(测试 harness 原来没有 dispatch loop、没有真 ELF 加载路径),逐层补了七个真实缺口,其中三个是潜伏 bug(这个 #DF 是最大的一个)。换句话说,「搭一个能跑真 ring3 程序的测试」这件事本身,就是一次系统性掏潜伏 bug 的体检。这是测试基建投资的典型回报:你以为在验功能,实际挖出的是藏着的错。

## 诚实的边界

**只静态链接,没动态链接。** 这一步的 musl 程序是把 `libc.a` 静态链进可执行文件,没有 `ldso`、没有 `PT_INTERP`、没有 `.so`。动态链接(内核加载 `ld-musl`、处理 `PT_INTERP`、运行时重定位)是后面的事(见 [006 · ELF 动态链接](../006/))。

**`execve` 替换路径的栈铺设是 follow-up。** `build_initial_stack` 这一章接的是 `launch_user_program`(首进程:execve 后自己 map 栈 + 跳 entry)这条路径。`sys_execve` 那条「替换当前进程镜像」的路径,栈铺设还没接(当前没有调用方走那条),是后续。所以现在 musl hello 是在 `launch_user_program` 路径上 boot 起来的,不是 shell 里 `execve("/hello")` 起来的。

**musl 工具链得在宿主机上先编一次。** `tools/musl/build-musl.sh` 下载 musl 1.2.6、用宿主 GCC 编出一个 sysroot(`libc.a` + crt 文件 + 头文件)。这是宿主机侧的一次性工作,不是内核运行时的事——但它依赖网络下载 musl 源、依赖宿主 GCC 版本(脚本里处理了 GCC16 的 `-fno-link-libatomic` 之类坑)。内核测试的 ring3 smoke 默认是关的(`-DCINUX_MUSL_HELLO_SMOKE=ON` 才编进 hello),因为编 musl sysroot 不是每次构建都该做的事。

**TLS / canary 依赖前面的弧。** musl 的 `__init_tp` 用 `arch_prctl(ARCH_SET_FS)` 设 `fs_base`(TLS),栈 canary 读 `%fs:0x28`——这些能工作,是因为 clone/TLS 和栈 canary 的种子(来自 `AT_RANDOM`,这一章铺的)都就位了。musl 移植是站在前面好几卷的地基上的,不是孤立的一步。

验证该看到什么,见配套 lab。下一章该把用户态再往前推——TTY 行规范、动态链接那一线。
