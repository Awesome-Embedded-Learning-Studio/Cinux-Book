---
title: 03 · SSE 初始化:一个只在 -O2 才炸的 Triple Fault
---

# SSE 初始化:一个只在 -O2 才炸的 Triple Fault

### SSE:一个只在 -O2 才炸的 Triple Fault

故事来了。给 kprintf 写 host 单测时,顺带把整个内核测试也用 Release(`-O2`)编了一遍,结果小内核在 IDT 初始化阶段 Triple Fault,QEMU 直接退出。诡异的是 `-O0`(Debug)下一切正常,而且之前 GDT、kprintf、C++ 运行时的测试全过。

定位它靠的是两个老办法。第一个是 **debugcon 标记法**:在 `idt_init` 的各个步骤之间,插一句 `outb $0xE9, '某字符'`,把执行进度打到 QEMU 的 debug 日志。结果日志停在 `idt_init` 的清零循环那一步,之后再无标记——崩溃点被精确钉死在「第一个会用上 SSE 指令的地方」。

第二个是 **反汇编对比**。`-O2` 把那个把 IDT 结构体数组清零的循环,向量化成了一串 SSE 指令:

```asm
; -O2 生成的 idt_init 清零循环
pxor   %xmm0, %xmm0          ; ← 第一条 SSE 指令,崩在这
movaps %xmm0, (%rcx,%rdx,1)  ; 16 字节对齐写入
```

而 `-O0` 生成的是逐字节的普通 store,根本不碰 SSE。这就解释了为什么只有 `-O2` 崩。

根因是控制寄存器。在崩溃前把 CR0、CR4 读出来打到 debugcon,看到 `CR4 = 0x...20`——只有 PAE 位被置上,而 OSFXSR(bit 9)是 0。按 Intel SDM 的规定,SSE2 这类 128 位指令在 `CR4.OSFXSR = 0` 时会触发 `#UD`(非法操作码,向量 6)。于是链条就清楚了:

```text
boot 入口 → cli →(从未设 CR4.OSFXSR)→ ... → idt_init
                                              ↓
                                        pxor %xmm0
                                              ↓
                                  CR4.OSFXSR = 0  →  #UD (vector 6)
                                              ↓
              可此刻 IDT 还没 lidt(limit = 0),连 #UD 自己都找不到 handler
                                              ↓
                                        Triple Fault → QEMU -no-reboot → exit(0)
```

这里有个容易误解的地方:64 位长模式**硬件上**确实支持 SSE,这是架构强制要求的。但「硬件支持」不等于「OS 已启用」——CPU 仍然要检查 `CR4.OSFXSR`,这位不设,128 位 SSE 指令就一律当非法指令处理。两者不冲突,只是很多人(包括我们之前的 boot 代码)默认以为进了长模式 SSE 就自动能用了,这是个常见误区。

另一个细节:为什么是 Triple Fault 而不是看到一个 `#UD` 的异常输出?因为 `pxor` 崩在 `idt_init` **内部**——IDT 正在被清零、还没 `lidt` 加载,此时 IDT 的 limit 还是 0。#UD 找不到 handler,又没法进一步处理,直接一路升级成 Triple Fault。这也解释了它为什么崩得那么「安静」:连异常处理都还没就绪,自然吐不出任何东西。

### 修复:在内核最早的入口把 CR4 拨好

修法很直接,但位置讲究。在 `kernel/mini/arch/x86_64/boot.S` 的 `_start`,紧跟在 `cli` 之后,把 SSE 相关的控制位一次性设好:

```asm
_start:
    cli

    /* Enable SSE: set CR4.OSFXSR (bit 9) and CR4.OSXMMEXCPT (bit 10) */
    movq %cr4, %rax
    orq $(1 << 9), %rax          /* OSFXSR: enable FXSAVE/FXRSTOR 管理 SSE 状态 */
    orq $(1 << 10), %rax         /* OSXMMEXCPT: 允许 SIMD 浮点异常传递为 #XF */
    movq %rax, %cr4
    clts                          /* 清 CR0.TS,不依赖 BIOS/KVM 的初始值 */
```

为什么放在 mini kernel 的 `boot.S` 而不是 big kernel 的 `main`?因为 `boot.S` 是整个内核链上**最早**的可执行点。`-O2` 可能在任何函数里生成 SSE 指令,越早把这位置好,后面所有代码——包括 mini kernel 自己、包括它加载的 big kernel——就都安全了。`clts` 顺手清掉 `CR0.TS` 也是同样的道理:不依赖 QEMU/BIOS 给 CR0 的初始值,把状态握在自己手里。

(完整版的排查过程——debugcon 标记法的具体输出、CR0/CR4 的读出值、`-O0` vs `-O2` 的指令对比表——我们另起一篇 debug-notes 收着,这里只走主线。)

## 调试现场沉淀的两条经验

这一章真正的「调试现场」就是上面那段 SSE 排查,它已经融在代码路线里讲了。这里只补两条这次沉淀下来的、以后会反复用到的经验。

一是 **debugcon**(port 0xE9)标记法值得当成看家手艺。内核崩得没声音的时候,在关键路径上插 `outb 0xE9` 打标记,再读 `debug.log`,是定位「崩在哪一步」最快的办法——比插 kprintf 强,因为 kprintf 本身可能还没初始化(这次就崩在 IDT 之前,串口虽然有,但更早的崩溃根本走不到 kprintf)。

二是「`-O0` 正常、`-O2` 崩」这种信号,要先怀疑某个硬件特性没被初始化。优化级别一变,代码就崩,根因往往是编译器在 `-O2` 下用上了某种 `-O0` 不用的指令(SSE/AVX、向量化内存操作、特定的寻址方式),而这些指令依赖某个没设好的控制位或状态。下次再撞上「换个优化级别就崩」,先往这个方向想,别急着怀疑自己的逻辑。
