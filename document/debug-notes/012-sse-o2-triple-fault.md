---
title: Debug · SSE 未初始化：一个 -O2 才炸的 Triple Fault
---

# Debug · SSE 未初始化：一个 -O2 才炸的 Triple Fault

> 出处：tag `012_driver_serial`，`document/notes/012/012-01-sse-init-crash-o2.md`。这里把它提炼成「症状 → 定位 → 根因 → 修复 → 防复发」的案例，不照抄原始笔记。

## 症状

用 `CMAKE_BUILD_TYPE=Release`（`-O2`）构建内核测试，小内核在 IDT 初始化阶段 Triple Fault，QEMU 直接退出：

```text
[INIT] Setting up GDT...
[INIT] GDT loaded successfully.
[INIT] Setting up IDT...
QEMU unexpected exit code: 0
```

几个特征凑在一起，基本排除了「单纯的逻辑写错」：「Setting up IDT...」之后串口再没吐过一个字；QEMU 退出码是 `0`，而 `isa-debug-exit` 的退出码公式是 `(value << 1) | 1`、恒为奇数，退出码 `0` 说明它根本不是从 isa-debug-exit 出来的，配合 `-no-reboot` 基本就是 Triple Fault；偏偏 `-O0`（Debug）构建又一切正常，之前的 GDT、kprintf、C++ 运行时测试也全过。

「换个优化级别就崩」这个信号，是后面整个排查的钥匙。

## 定位

崩得没声音，不能靠 kprintf——它走的串口没问题，问题是崩在 IDT 还没建好的地方，连异常都处理不了。这时候用 **debugcon 标记法**：在关键步骤之间插一句 `outb $0xE9, <字符>`，把执行进度打到 QEMU 的 debug 日志。

```text
debug.log: OPLJ1234...0    ← 停在某个 '0' 标记之后，再无输出
```

崩溃点被钉死在 `idt_init()` 的清零循环——也就是「第一个会用上 SSE 指令的位置」。

接着做 **反汇编对比**，把 `-O0` 和 `-O2` 生成的 `idt_init` 摆在一起：

| 构建类型 | 清零循环生成方式 | 是否用 SSE | 结果 |
|----------|------------------|-----------|------|
| `-O0` | 逐字节 / 逐字段 `mov` store | 否 | 正常 |
| `-O2` | `pxor %xmm0,%xmm0` + `movaps` 向量化 | 是 | 崩 |

```asm
; -O2 生成
ffffffff800239e3:   pxor   %xmm0,%xmm0          ; ← 第一条 SSE 指令，崩在这
ffffffff800239ee:   movaps %xmm0,(%rcx,%rdx,1)   ; 16 字节对齐写入
```

最后在崩溃前把 `CR0`、`CR4` 读出来打到 debugcon：

```text
CR0 = 0x80000011    → PG=1, PE=1, ET=1（TS=0, EM=0）
CR4 = 0x00000020    → PAE=1（OSFXSR=0, OSXMMEXCPT=0）
```

`CR4` 只有 PAE 被置上——`OSFXSR`（bit 9）是 0。嫌疑人锁定了。

## 根因

按 Intel SDM，`PXOR`/`MOVAPS` 这类 128 位 SSE2 指令，当 `CR4.OSFXSR = 0` 时触发 `#UD`（非法操作码，向量 6）。这条规则在 64 位长模式下照样适用。

这里有个常见误区要破：**64 位长模式硬件上确实支持 SSE**（架构强制要求），但「硬件支持」不等于「OS 已启用」。CPU 照样要查 `CR4.OSFXSR`，这位不设，128 位 SSE 指令一律当非法指令处理。我们之前的 boot 代码只设了 `CR4.PAE` 进长模式，从没设 OSFXSR，只是 `-O0` 下没人用 SSE，bug 才一直没暴露。

崩溃链于是完全自洽：

```text
boot 入口 → cli（从未设 CR4.OSFXSR）→ … → idt_init
                                           ↓
                                     pxor %xmm0,%xmm0
                                           ↓
                              CR4.OSFXSR = 0  →  #UD (vector 6)
                                           ↓
            可此刻 IDT 还没 lidt（limit=0），#UD 自己都找不到 handler
                                           ↓
                                     Triple Fault → QEMU -no-reboot → exit(0)
```

这也解释了为什么崩得「安静」：`pxor` 崩在 `idt_init` **内部**，IDT 正在被清零、还没加载，#UD 找不到落点，直接升级成 Triple Fault，连一句异常信息都吐不出来。

## 修复

在 `kernel/mini/arch/x86_64/boot.S` 的 `_start`，`cli` 之后立即把 SSE 相关的控制位设好：

```asm
_start:
    cli

    /* Enable SSE */
    movq %cr4, %rax
    orq $(1 << 9), %rax          /* OSFXSR: enable FXSAVE/FXRSTOR */
    orq $(1 << 10), %rax         /* OSXMMEXCPT: allow SIMD #XF */
    movq %rax, %cr4
    clts                          /* clear CR0.TS */
```

为什么放在 mini kernel 的 boot 入口、而不是 big kernel 的 main？因为 boot.S 是整条内核链上**最早**的可执行点。`-O2` 可能在任何函数里生成 SSE 指令，越早把位置好，后面所有代码（mini kernel 自己、以及它加载的 big kernel）就都安全。`clts` 清 `CR0.TS` 同理——不依赖 QEMU/BIOS 给 CR0 的初始值，状态握在自己手里。

修完后 22 项内核测试全过，`-O2` 构建稳稳进 idle。

## 防复发

最划算的防复发，是把「引导期 SSE 初始化」固化成一段对照清单，每次新 boot 路径都照着过一遍。x86_64 内核入口该设的位是固定的：

```text
CR0: 清 EM(bit 2)、清 TS(bit 3)、设 MP(bit 1)
CR4: 设 OSFXSR(bit 9)、设 OSXMMEXCPT(bit 10)
```

对应就是一段 `mov %cr4 / or / mov %cr4` + `mov %cr0 / and,or / mov %cr0` + `clts`，少设一位，迟早会被某个 `-O2` 用上对应特性的函数炸出来。

再往上，值得养成两个条件反射。其一是看到「`-O0` 好、`-O2` 崩」就先查有没有未初始化的硬件特性被编译器用上——优化级别一变代码就崩，根因往往是 `-O2` 启用了 `-O0` 不用的指令（SSE/AVX、向量化内存操作、特定寻址），而这些指令依赖某个没设好的控制位或状态，先往这想，再怀疑逻辑。其二是早期没输出的崩溃，就用 **debugcon**（port `0xE9`）打标记——内核崩在 kprintf/IDT 之前、串口指望不上时，在路径上插 `outb 0xE9`、读 debug 日志，是定位「崩在哪一步」最快的办法，它比插 kprintf 强，因为 kprintf 可能还没初始化。

最后，QEMU 的退出码本身也是个信号：`isa-debug-exit` 正常退出恒为奇数（`(value<<1)|1`），退出码 `0` 配合 `-no-reboot` 几乎可以断定是 Triple Fault，看到 `exit 0` 先往三重故障想，别以为是「正常结束」。

## 参考

- Intel SDM Vol.3（控制寄存器）：`CR4.OSFXSR`(bit 9)、`CR4.OSXMMEXCPT`(bit 10)、`CR0.TS/EM/MP`。本地 PDF，可 `pdf-reader` 搜 "OSFXSR" 复核。
- Intel SDM Vol.2：SSE2 指令在 `CR4.OSFXSR = 0` 时 `#UD`；`CLTS`、`FXSAVE`/`FXRSTOR`。
- OSDev — QEMU isa-debug-exit：退出码 `(value<<1)|1` 恒奇数，`0` 在 `-no-reboot` 下为 Triple Fault。
- 原始排查笔记：[012-01-sse-init-crash-o2.md](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/document/notes/012/012-01-sse-init-crash-o2.md)。
