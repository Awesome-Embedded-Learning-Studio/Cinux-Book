---
title: Lab 004 · musl 静态移植验证
---

# Lab 004 · musl 静态移植验证

> 对应 `document/book/07-userland/004/`。验证分两层:**内核侧**(Linux ABI + 初始栈 + 新 syscall,靠 `test_initial_stack` host 单测 + `run-kernel-test` 验,随时能跑)和 **musl 端到端**(用 `build-musl.sh` 编 sysroot、跑 musl hello 的 ring3 smoke,需要先在宿主机编一次 musl)。前者是「内核准备好接住 musl 了」的客观证据;后者是 A 档 punchline(真 musl 程序跑起来),但依赖 musl 工具链。

## 目标

确认四件事:

1. `SYS_chdir`/`SYS_brk` 撞号修了(chdir=80、brk=12),`jump_to_usermode` 的 #DF 修了(`movq $0x202,%r11`);
2. 初始栈铺了对——`test_initial_stack` host 单测验 auxv + 16 字节对齐;
3. `run-kernel-test` 954/0 不回归(新 syscall + 初始栈测试都绿);
4. (可选,musl 端到端)`build-musl.sh` 编出 sysroot、ring3 smoke 跑 musl hello 出 `Hello from musl`。

## 步骤

### 1. 撞号 bug + #DF 修法(内核侧)

先核两个真 bug 都修了:

```bash
grep -n "SYS_brk\|SYS_chdir" kernel/syscall/syscall_nums.hpp
```

应看到 `SYS_brk = 12`、`SYS_chdir = 80`(注释会写「was wrongly 12, collided with brk」)。撞号没了。

```bash
sed -n '90,98p' kernel/arch/x86_64/usermode.S
```

应看到 `jump_to_usermode` 里设 RFLAGS 那行是 `movq $0x202, %r11`(立即数加载,**不碰内存**)——不是 `pushq $0x202; popq %r11`(那个会在切到用户栈后内核态写用户内存、SMAP 下 #PF→#DF)。注释会说明这是 SYSRET 恢复的 RFLAGS。

> **思考这个 bug 为什么潜伏**:`pushq/popq` 那版写用户内存,只有 SMAP 开了才会被拦。本机 WSL2 不透传 SMAP CPUID(见 056 的边界),所以 SMAP 没生效、bug 没发作。真机/SMAP 透传环境必 #DF。这是「开发机绿 ≠ 真机对」的典型。

### 2. 初始栈:host 单测验 auxv

```bash
cmake --build build -j$(nproc) --target test_initial_stack && \
build/test/test_initial_stack 2>&1 | tail -10
```

应看到两个测试 PASS:`initial_stack: argc/argv/envp layout and 16-byte RSP alignment`(栈布局 + 入口 RSP 16 字节对齐)和 `initial_stack: auxv entries incl AT_RANDOM/AT_EXECFN, AT_NULL terminator`(辅助向量铺对了)。这是 musl 启动能读到 auxv 的客观保证——不依赖 QEMU、不依赖 musl,纯栈布局逻辑。

看 helper 铺的布局:

```bash
sed -n '3,21p' kernel/proc/initial_stack.hpp
```

应看到 `[ argc | argv[] | NULL | envp[] | NULL | auxv.. | AT_NULL | strings.. | pad ]` 这条 Linux 标准 initial-stack 布局,以及 musl 硬依赖的 AT_*(`AT_PHDR`/`AT_RANDOM`/`AT_UID`...)清单。

### 3. run-kernel-test 不回归

```bash
cmake --build build --target run-kernel-test 2>&1 | grep -E "=== Tests: [0-9]+ passed, 0 failed ===|ALL TESTS PASSED" | tail -2
```

应是 `954 passed, 0 failed` + `ALL TESTS PASSED`(含本章新增的 syscall / initial-stack 相关测试)。新 syscall(`sys_open`/`sys_stat`/`sys_set_tid_address`)在 `kernel/syscall/` 下,handler 注册进表。

### 4. (可选)musl 端到端:跑 musl hello

这一步需要先在宿主机编 musl sysroot(下载 musl 1.2.6 + 宿主 GCC 编):

```bash
bash tools/musl/build-musl.sh        # 编出 build/musl-sysroot/lib/libc.a
bash tools/musl/build-hello.sh       # 用 musl-gcc 编 hello.c → 静态 ELF
```

编出 sysroot 后,开 ring3 smoke 重编内核测试:

```bash
cmake -S . -B build -DCINUX_MUSL_HELLO_SMOKE=ON && \
cmake --build build --target run-kernel-test 2>&1 | grep -aE "Hello from musl|smoke.*PASS|sys_exit" | head
```

应看到 `Hello from musl on CinuxOS!` + `[SYSCALL] sys_exit(0)` + `smoke: hello exit_status=0 ... PASS`。这是 A 档 punchline:整条 musl 启动链在 ring3 端到端跑通。

> **这一步依赖 musl 工具链**(网络下载 musl 源 + 宿主 GCC 版本匹配,`build-musl.sh` 里处理了 GCC16 的 `-fno-link-libatomic` 坑)。编不出 sysroot 不代表内核没准备好——前 3 步已经客观证明了内核侧的 ABI + 初始栈 + syscall 就绪。这一步是「真 musl 程序端到端」的额外验证,需要工具链就位。

## 验收清单

- [ ] `SYS_chdir=80`/`SYS_brk=12`(撞号修了);`jump_to_usermode` 用 `movq $0x202,%r11`(#DF 修了,不碰用户内存)。
- [ ] `test_initial_stack` 两个测试 PASS(栈布局 + 16 字节对齐 + auxv);run-kernel-test 954/0。
- [ ] 知道 #DF bug 是被 WSL2 的 SMAP-不透传挡住的潜伏 bug——真机会发作。
- [ ] (可选)musl hello ring3 smoke 出 `Hello from musl` + 干净 exit(需 musl sysroot)。

## 别做这些

- **别**以为开发机绿就是真机对——`jump_to_usermode` 那个 #DF 在 WSL2(SMAP 没透传)上不发作,真机/SMAP 透传环境必炸。「环境限制掩盖真 bug」要警惕。
- **别**硬编码 syscall 号在用户壳里——`user/libc/syscall.cpp` include `kernel/syscall/syscall_nums.hpp` 用 enum,单一事实源,改号一处同步。
- **别**以为 `execve` 替换路径也铺了初始栈——`build_initial_stack` 这一章只接了 `launch_user_program`(首进程)路径;`sys_execve` 替换路径的栈铺设是 follow-up。
- **别**以为这是动态链接——musl 程序静态链 `libc.a`,没有 `ldso`/`PT_INTERP`/`.so`。动态链接是后面的事。
- **别**跳过 `test_initial_stack` 直接跑 musl hello——host 单测是「栈布局对不对」的确定性验证(不依赖 QEMU/musl),比端到端 smoke 脆性低。先验内核侧,再跑端到端。
