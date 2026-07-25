---
title: 09 · 调试现场:sys_creat 首次深入 ext2 触发 GPF
---

# 调试现场:sys_creat 首次深入 ext2 触发 GPF

## 调试现场:sys_creat 第一次深入 ext2 时,触发了 General Protection Fault

这一章最有意思的一个坑,不在 ext2 逻辑里,而在它第一次被系统调用真正驱动起来的时候。过程值得完整走一遍,因为它是一个经典的「症状在深处、根因在入口」的对齐 bug。

**现象。** 把 006 的写链路接好,头几次 `touch`、`mkdir` 都正常。直到在 QEMU 里执行 `touch /hello2.txt`,内核直接崩:

```text
==== EXCEPTION: #GP (vector 13) ====
  RIP   = 0xFFFFFFFF81005D97   CS  = 0x0010
  RFLAGS= 0x0000000000010002
  ERROR CODE = 0x0000000000000000
========================================
```

General Protection Fault(#GP,向量 13),错误码 0。错误码 0 通常意味着「不是因为段选择子问题,而是别的通用保护违例」——比如一条要求对齐的指令碰上了没对齐的地址。

**定位崩溃点。** 用 `nm` 把崩溃地址 `0x5D97` 反查回符号:

```text
ffffffff81005d20 T Ext2::create(uint32_t, char const*, uint32_t)
```

RIP 落在 `Ext2::create()` 内部。再反汇编那个地址附近:

```text
ffffffff81005d7d:  lea    0x80(%rsp),%rdx    ; 栈上 new_disk 的地址
ffffffff81005d8e:  mov    %rdx,%rax
ffffffff81005d97:  movaps %xmm0,(%rax)       ; ← #GP 就在这一条
```

`movaps` 是一条 SSE 指令,把一个 16 字节的 XMM 寄存器存到内存,**要求目标地址 16 字节对齐**。`%rax = %rsp + 0x80`,而当时 `%rsp` 末尾是 `...78`,`0x78 % 16 = 8`——没对齐。`movaps` 一执行就 #GP。

这条 `movaps` 是哪来的?正是 `Ext2::create` 里那段把新 `Ext2Inode` 清零的循环(就是上一节我们标出来的那句)。编译器发现「把一个结构体清零」可以用一条 `movaps` 把 16 字节的 `xmm0`(全 0)一次写进去,比一字节一字节快,于是这么生了码。代码没错,错的是**栈没对齐到 16 字节**。

**根因:syscall 入口没维护栈对齐。** 沿调用链往上回溯:`syscall_entry`(汇编)→ `syscall_dispatch`(C)→ ... → `Ext2::create`(C)。System V AMD64 ABI 有一条硬性要求:**在执行 `call` 指令的那一刻,RSP 必须是 16 的倍数**。`call` 自己会 push 8 字节返回地址,所以被调用者一进来时 RSP 是 `16k+8`,但它只要在每次 `call` 别人之前把栈重新对齐到 16 就行。这套约定保证了任何函数里,栈上局部变量的地址都能满足 `movaps` 这类指令的对齐要求——前提是**从入口开始,每一层都守规矩**。

Cinux 的 `syscall_entry` 是手写汇编,它没守。它 push 了 12 个寄存器构造 trap frame(96 字节),再 push 第 7 个 C 参数(8 字节),一共 13 次 push = 104 字节。`104 % 16 = 8`。于是在 `call syscall_dispatch` 之前,RSP 是 `16k+8` 而不是 `16k`——**差了 8 字节**。这个 8 字节的偏移一路传下去,到了 `Ext2::create` 里,栈上 `new_disk` 的地址就都偏了 8,本来该对齐的变得不对齐,`movaps` 一碰就炸。

**为什么之前的系统调用没事?** 这是最关键的问题,也是这类 bug 最坑的地方。023/004 那些 syscall(`sys_read`/`sys_write`/`sys_open` 等)执行路径浅、调用链不深,而且没碰上要求 16 字节对齐的指令。对齐是错的,但「错而未爆」。`sys_creat` 是第一个**深入调到 ext2 复杂逻辑、且那里恰好有结构体清零**的 syscall,这才头一次把这条潜伏的对齐错误逼了出来。换句话说:bug 不在崩溃的 `Ext2::create` 里,而在最顶层的 `syscall_entry` 里;只是直到这里才暴露。

**修复。** 在 push 第 7 个参数之前,先 `subq $8, %rsp` 把栈补齐到 16:

```asm
    movq 72(%rsp), %rax                # 取出第 7 个参数
    subq $8, %rsp                      # ← 新增:把栈对齐到 16 字节
    pushq %rax                         # push 第 7 个参数

    movq 40(%rsp), %rdi                # 注意:所有偏移都 +8 变成 +16
    ...
    call syscall_dispatch
    addq $16, %rsp                     # 清理:参数 + 对齐填充,从 $8 改成 $16
```

多垫的那 8 字节,让 trap frame 总大小从 104 变成 112(`112 % 16 = 0`),`call` 前 RSP 终于对齐了。代价是 trap frame 里所有字段的读取偏移都得 `+8`(`movq 32(%rsp)` 变成 `movq 40(%rsp)`,以此类推),清理时的 `addq $8` 也得改成 `addq $16`。

**教训。** 三条,都值得记进经验:

1. **手写的入口(syscall、中断处理)必须维护 ABI 的栈对齐不变量。** SysV AMD64 假定 `call` 前 RSP ≡ 0 (mod 16)。任何手写汇编入口,只要它最后要 `call` 一段 C 代码,就得自己保证这一点,否则所有用 SSE/AVX 对齐指令的编译产物都可能随机崩。
2. **对齐 bug 有隐蔽性。** 崩在 `Ext2::create`,根因在 `syscall_entry`;调用浅的 syscall 不爆,只有调用深、且碰上对齐敏感指令时才暴露。看到 #GP + error_code=0,第一反应就该是「是不是哪里没对齐」。
3. **排查路径是固定的。** #GP err=0 → 反汇编 RIP,找 `movaps`/`movdqa` 这类对齐指令 → 算它目标地址对不对齐 → 沿调用链回溯到最顶层的汇编入口,数 push 次数、算 RSP 对齐状态。
