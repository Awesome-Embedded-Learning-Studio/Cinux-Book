---
title: Lab 007 · 第一次能被打断
---

# Lab 007 · 第一次能被打断

> 这个 lab 配套 [007 · 第一次能被打断](../../book/02-mini-kernel/007-mini-kernel-intr.md)。目标:给 mini kernel 装自己的 GDT 和 IDT,写 ISR stub,接住 `int $3`(#BP)和页错误(#PF),dump 寄存器后活着继续。**GDT 描述符自己拼位、IDT 门自己编码、stub 压栈顺序自己定**,不给现成答案。

## 实验目标

- 写 3 项 GDT(null/code64/data64)+ `gdt_init`(`lgdt` + 远返回刷 CS + 重载数据段)。
- 写 IDT 结构(16 字节门、InterruptFrame)+ `idt_init`,配 #BP(陷阱门)和 #PF(中断门)。
- 写 ISR stub 两宏(无错误码补 0 / 有错误码),保存现场、传 `InterruptFrame*`、`iretq` 返回。
- 写 C handler:`handle_bp` dump 后返回;`handle_pf` 读 CR2、解错误码位、dump。
- main 里 `gdt_init → idt_init → int $3`,验证被接住、内核存活;配 host 编码单测。

## 前置条件

- 完成 [Lab 006](lab-006-mini-kernel-pmm.md):内核有 kprintf、PMM、QEMU 测试框架。
- 理解 GDT 描述符位布局、IDT 门编码、`iretq` 栈帧(rip/cs/rflags/rsp/ss)。

## 任务分解

分五块走。

第一块,GDT。定义 `GdtEntry`(8 字节 packed)、`GdtPointer`,写 `make_gdt_entry(base,limit,access,flags)` 拼字节;填三项(code64 `0x9A`/`0x0A`、data64 `0x92`/`0x0C`);`gdt_init` 里 `lgdt`,然后用"push CS + push 返回地址 + lretq"刷新 CS,再 `mov` 重载 DS/ES/FS/GS/SS。

第二块,IDT。定义 `IdtEntry`(16 字节)、`IdtPointer`、`InterruptFrame`(r15..rax/error_code/rip/cs/rflags/rsp/ss)。写 `set_idt_entry(vector,handler,selector,type_attr,ist)` 把 handler 地址拆三段填。`idt_init` 清 256 项后配 #BP(`0x8F`)、#PF(`0x8E`),selector=`SEGMENT_CODE64`,`lidt`。

第三块,ISR stub。写两宏:`ISR_NOERRCODE`(先 `push $0` 补伪错误码)和 `ISR_ERRCODE`(不补)。都 push rax..r15、`mov %rsp,%rdi`、`call handler`、pop r15..rax、`add $8,%rsp`、`iretq`。实例化 `isr_bp_stub`(→handle_bp)、`isr_pf_stub`(→handle_pf)。

第四块,C handler。`dump_interrupt_frame` 打全部寄存器;`handle_bp` dump 完返回;`handle_pf` 用 `mov %%cr2` 取缺页地址、解 error_code 的 P/W/U/RSVD/I 位、dump。

第五块,接线和测试。main 里 `gdt_init`→`idt_init`→(PMM)→`int $3`→打印"continued"。配 `test/unit/test_gdt_idt.cpp`(host 单测验编码),`test/test_interrupts.cpp`(QEMU 触发异常验存活)。

## 接口约束

这些得自己保证对、lab 不给现成代码:

- 选择子:`SEGMENT_CODE64=0x08`、`SEGMENT_DATA64=0x10`。
- GDT 描述符:code64 `access=0x9A flags=0x0A`(L=1)、data64 `access=0x92 flags=0x0C`;base 全 0、limit `0xFFFFF`。
- IDT gate:#BP `type_attr=0x8F`(陷阱门,P=1 DPL=0)、#PF `type_attr=0x8E`(中断门);selector=`0x08`;IST=0;向量 3 和 14。
- ISR 压栈:先 push rax(最高地址)、最后 push r15(`rsp` 指向 r15),配 `InterruptFrame` 从 r15 起的字段顺序;压入与弹出严格相反。
- `ISR_NOERRCODE` 必须先 `push $0` 补伪错误码,末尾 `add $8,%rsp` 抵消;`ISR_ERRCODE` 不补(CPU 已压),末尾同样 `add $8`。
- `#PF` 缺页地址从 `CR2` 读(`mov %%cr2`)。
- **全程不开 `sti`**:`int $3` 是同步异常不需要开中断;没有 PIC,开了反而三重故障。

## 验证步骤

host 编码单测(第一道):

```bash
cmake --build build --target test_host
```

`test_gdt_idt` 验描述符字节、gate 编码、选择子。

QEMU 内核测试(第二道):

```bash
cmake --build build --target run-kernel-test
```

`test_interrupts` 触发异常、handler dump、内核存活。

量产看效果:

```bash
cmake --build build --target run
```

串口出现 `GDT loaded`、`IDT loaded`、`Triggering breakpoint exception (int $3)...`、`==== EXCEPTION: #BP (vector 3) ====` 带寄存器块、最后 `Breakpoint test passed! Execution continued after #BP.`。

## 常见故障

触发异常就三重故障重启、连 dump 都没有。IDT 没 `lidt` 或没建,或 GDT/IDT 顺序反了(IDT 引用的选择子无效)。必须 GDT 先、IDT 后,且都在 `int $3` 之前。

dump 出来的寄存器错位(RAX 显示成 RBX)。stub 压栈顺序和 `InterruptFrame` 字段顺序对不上。严格 rax 先压、r15 最后压,配结构体从 r15 起。

#PF dump 的读/写或地址不对。错误码位读反,或 CR2 没读。`mov %%cr2` 取地址、对照 SDM 位定义解 error_code。

handler 返回后内核跑飞。stub 末尾忘了 `add $8,%rsp` 跳错误码,或压入/弹出数量不等,`iretq` 弹错了栈。核对 push/pop 一一对应、`add $8` 在位。

`sti` 后立刻三重故障。没有 PIC 驱动就开了硬件中断。这一章全程不开 `sti`,只靠同步的 `int $3` 演示。

## 通过标准

- `test_host` 里 `test_gdt_idt` 编码全过。
- `run-kernel-test` 的 `test_interrupts` 触发异常后内核存活、退出码 0。
- 量产 `make run` 看到 `#BP` 的寄存器 dump 和 `Execution continued after #BP`。
- 全程无 `sti`、无 PIC;handler 只 dump 不修页——那是后面的 lab。
