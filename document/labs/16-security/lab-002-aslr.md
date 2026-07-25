---
title: Lab 002 · ASLR 三件套随机化验证
---

# Lab 002 · ASLR 三件套随机化验证

> 对应 `document/book/16-security/002/`。验证档 **B 档**(机制,无用户可见现象,靠抽样测试 + grep 验证)。本章给用户态布局加 ASLR:栈顶 / mmap 起点 / brk 起点各随机一个页对齐偏移;代码段(text)没随机化(non-PIE 绝对寻址挡着)。验证核心三件事——三个 offset 函数页对齐 + 落在区间 + 每次不同、三个消费点真用了 offset、代码段确实没动。

## 目标

确认四件事:

1. `aslr.hpp` 三个 offset 函数的掩码对(栈 `0x7FF000`/mmap `0x3FFFF000`/brk `0xFFF000`),保证页对齐;
2. 三个消费点(栈顶 `user_launch.cpp`、mmap `sys_mmap.cpp`、brk `execve.cpp`)真调了对应 offset;
3. `test_aslr` 抽样测试 PASS(页对齐 + 区间内 + 多次抽样非全同 → 证明 PRNG 在推进);
4. 知道**代码段没随机化**(ELF 仍是 `ET_EXEC` 钉在 `0x400000`),别误以为 ASLR 全覆盖。

## 步骤

### 1. 三个 offset 函数:掩码 + 页对齐

```bash
sed -n '36,52p' kernel/lib/aslr.hpp
```

应看到三个 `inline` 函数,各 `g_random.next64() & <mask>`:栈 `& 0x7FF000`(0..8 MiB)、mmap `& 0x3FFFF000`(0..~1 GiB)、brk `& 0xFFF000`(0..16 MiB)。三个掩码末尾都是 `000`——这是页对齐(4 KiB 整倍数)的硬要求,不是随便选的数(破坏栈 16 字节对齐 ABI 会让用户程序在 SSE 指令上崩)。

### 2. 三个消费点真用了 offset

```bash
# 栈顶
grep -n 'aslr_stack_offset' kernel/proc/user_launch.cpp
# mmap
grep -n 'aslr_mmap_offset' kernel/syscall/sys_mmap.cpp
# brk
grep -n 'aslr_brk_offset' kernel/proc/execve.cpp
```

栈顶应在 `user_launch.cpp:44`(`stack_top = USER_STACK_TOP - aslr_stack_offset()`),mmap 在 `sys_mmap.cpp:117`(`hint = USER_MMAP_BASE + aslr_mmap_offset()`),brk 在 `execve.cpp:344`(`brk_gap = aslr_brk_offset()`)。注意 mmap 那处只在**非 `MAP_FIXED`** 分支随机——用户指定了地址(`MAP_FIXED`)就不随机,内核自己挑时才随机。

### 3. test_aslr 抽样测试(B 档的核心验证)

ASLR 没法靠「触发一次攻击看它迷路」验证,靠的是抽样测试:调 offset 函数几十次,断言每次结果都页对齐、落在区间内、而且多次抽样不全相同(全同 = PRNG 没推进,坏了)。

```bash
cmake --build build --target run-kernel-test 2>&1 | grep -iE "aslr|test_aslr" | head
```

应看到 test_aslr 的几项(range / page-align / non-identical)**PASS**。这个「多次抽样非全同」是关键——它间接验证了 KRandom 的 `next64` 在推进、非零、在变(KRandom 没有独立的 PRNG 数学单测,靠这个间接验)。最终 `942 passed, 0 failed` + `ALL TESTS PASSED`。

### 4. 代码段确实没随机化(别误判)

这是诚实的边界,值得核一下:

```bash
# shell 是 non-PIE(ET_EXEC),入口钉在 0x400000
readelf -h build/user/shell 2>/dev/null | grep -E "Type|Entry"
# 看一条绝对立即数寻址(全局/字符串地址烤死在指令里)
objdump -d build/user/shell 2>/dev/null | grep -m2 '0x40[0-9a-f]'
```

`Type: EXEC`(ET_EXEC,不是 ET_DYN/PIE)、`Entry: 0x400270`,以及反汇编里 `mov $0x40????,%...` 这种 32 位绝对立即数——这就是代码段不能随机化的根因:base 一挪,这些绝对引用全错位。所以 ASLR 覆盖的是栈/mmap/brk,**不含 text**。

> **思考**:为什么 text 随机化是「死路」而不是「没做」?——small model + non-PIE 把全局/字符串地址编成绝对立即数(99 处绝对引用、0 处 RIP-relative)。要随机化 text 得先改 PIE(ET_DYN + RIP-relative)+ 内核 ELF loader 处理动态重定位(`R_X86_64_RELATIVE`),不是给 base 加个 offset 就行。

## 验收清单

- [ ] `aslr.hpp` 三 offset 掩码 `0x7FF000`/`0x3FFFF000`/`0xFFF000`,末尾 `000` 保页对齐。
- [ ] 三消费点 `user_launch.cpp:44` / `sys_mmap.cpp:117` / `execve.cpp:344` 各调对应 offset(mmap 仅非 MAP_FIXED)。
- [ ] `test_aslr` 全 PASS(页对齐 + 区间 + 多次抽样非全同);run-kernel-test 942/0。
- [ ] `readelf` 确认 shell 仍 `ET_EXEC` 钉 `0x400000`——知道 text 没随机化,别误判 ASLR 全覆盖。

## 别做这些

- **别**以为 ASLR 覆盖了代码段——text 仍钉在 `0x400000`(non-PIE 绝对寻址),栈/mmap/brk 随机了但 ROP gadget 位置可预测。堵 text 要先 PIE 迁移。
- **别**指望 `make run` 直观看到「stack_top 每次不同」——GUI 模式 shell 不自动启动,不触发 `launch_user_program`。栈 offset 的随机性靠 `test_aslr` 验;想看端到端跑非 GUI 构建(自动 fork shell)。
- **别**把 KRandom 当 CSPRNG——它是 xoshiro256** + 启动熵混合的「够用 PRNG」,给 ASLR 搅地址行,别拿它生成加密密钥。
- **别**给 offset 用非页对齐的掩码——会破栈 16 字节对齐 ABI,用户程序跑 SSE 指令会崩。三个掩码末尾的 `000` 是 ABI 安全的硬要求。
