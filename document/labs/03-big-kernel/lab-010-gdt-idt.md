---
title: Lab 010 · 大内核 GDT/IDT
---

# Lab 010 · 大内核 GDT/IDT

> 这个 lab 配套 [010 · GDT](../../book/03-big-kernel/010-big-kernel-gdt.md) 和 [010b · IDT 与异常](../../book/03-big-kernel/010b-big-kernel-idt-exceptions.md) 两篇。目标是亲手把 big kernel 的异常通路从零搭起来,让它能接住 `int $3`、dump 出寄存器、活着继续跑。这里只给任务、约束和验证手段,**不贴现成答案**——选择子得自己算,stub 得自己写。

## 实验目标

给你的 big kernel 装上一套完整的异常兜底:初始化 GDT(5 段 + TSS)、初始化 IDT(14 个异常向量)、写好 ISR stub 和 C handler,最终在 main 里 `int $3` 能被接住、串口打印寄存器 dump、内核存活继续执行。

## 前置条件

- 已完成 009:big kernel 能被 mini kernel 加载、能跑 `kprintf` 往串口输出。
- QEMU + 工具链就绪,`make run` 能看到 big kernel 的启动信息。

## 任务分解

这一步分五块走,别想着一口吃下。

### 第一块:把 GDT 填出来

你要一个 7 entry 的表:null、内核代码、内核数据、用户代码、用户数据、TSS(占两槽)。每个段描述符的 access 和 flags 得自己按位算——算完可以对照一下,long mode 内核代码段应该是 `access=0x9A`、`flags=0xA`(G=1, L=1),其余的照着改属性位。算好填进 entries 数组,`gdtr` 指过去,limit 设成 `sizeof(entries)-1`。

### 第二块:实现 GDT::load

`lgdt` 把表地址告诉 CPU 之后,得刷新段寄存器。这里有个坑:`CS` 不能用 `mov` 改,只能靠远跳或远返回。你得构造一个"压目标 CS + 压返回地址 + `lretq`"的序列把 CS 切过来,然后 `mov` 刷新 DS/ES/FS/GS/SS,最后 `ltr` 把 TSS 选择子装进 TR。

### 第三块:写 ISR stub

汇编里准备两个宏:一个给"没有 error code"的异常(主动 `push $0` 凑齐布局),一个给"CPU 已经压了 error code"的异常。两宏都干同一件事——保存 rax..r15、把 `%rsp` 当第一个参数、`call` 对应 C handler、恢复寄存器、`addq $8` 跳过 error code、`iretq`。先想清楚哪些异常有 error code:#DF、#TS、#NP、#SS、#GP、#PF。

### 第四块:挂 IDT

给每个异常向量填一个 gate:offset 拆三段填 handler 地址、selector 填内核代码段、type_attr 用 `make_idt_attr` 算。gate 类型要分:#BP、#DB 用陷阱门(Trap, 0xF),其余用中断门(Interrupt, 0xE);特权上 #BP 设成 User(DPL=3),好让用户态也能 `int $3`。建议用一张路由表配一个循环,别堆 14 段重复代码。

### 第五块:写 C handler

至少要能 dump 出 InterruptFrame 里的全部寄存器。策略分两类:#BP、#DB 打完返回(让内核接着跑);其余 dump 完 `cli; hlt` 死在原地。#PF 额外要从 CR2 读出缺页地址、解析 error code 各位含义再打印。

## 接口约束

下面这些是你得自己保证对、但这里不给现成代码的东西,照着核:

- 选择子:`GDT_KERNEL_CODE=0x08`、`GDT_KERNEL_DATA=0x10`、`GDT_USER_CODE=0x1B`、`GDT_USER_DATA=0x23`、`GDT_TSS=0x28`。
- 尺寸:段描述符 8 字节、TSS 104 字节、IDT gate 16 字节——用 `static_assert` 钉死,别留到运行时炸。
- InterruptFrame 的布局由 stub 压栈顺序决定:r15、r14……rax、error_code、rip、cs、rflags、rsp、ss。C handler 拿到的就是这个结构的指针。
- gate 编码:内核中断门 `0x8E`、用户陷阱门(#BP)`0xEF`。

## 验证步骤

搭完之后,两套验证一起上。

先跑 QEMU in-kernel 测试:`make run-big-kernel-test`。它替你检查段寄存器值对不对、`int $3` 后内核有没有活着、连续触发多次异常状态腐不腐坏、gate 编码 `0x8E`/`0xEF` 算得对不对。退出码 1 是全过,3 是有挂的。

再手动看效果:`make run`,你应该在串口依次看到 `[BIG] GDT loaded.`、`[BIG] IDT loaded.`,然后 `==== EXCEPTION: #BP (vector 3) ====` 带一堆寄存器,最后一句 `[BIG] Breakpoint returned, continuing.`——这最后一句就是整个 lab 的通过信号。

## 常见故障

几个大概率会踩的坑,先给你提个醒。

要是 IDT 在 GDT 之前 init,gate 里填的 selector 是无效的,`int $3` 一进来就 #GP。顺序是死的:GDT 先,IDT 后。

ISR stub 里那个 `push $0` 别漏。漏了的话,"没有 error code"的异常其栈布局就和"有 error code"的不一致,C handler 读 InterruptFrame 时所有寄存器整体错位——你会看到 RAX 显示成了 RBX 的值,还以为硬件抽风。这个 bug 极其难调,别给自己埋。

main 里 `int $3` 之前千万别 `sti`。我们还没有 IRQ handler,PIT 定时器中断一来没人接,直接 Double Fault 重启。这一步不 sti 是 lab 故意留的边界,别手滑提前开。

段描述符的 access byte 算错一位,`lgdt` 之后行为就全乱,#GP 是轻的,三重故障是常态。算完建议对着 Intel SDM 的段描述符位定义再核一遍,别凭感觉。

## 通过标准

- `make run-big-kernel-test` 退出码 1,测试全绿。
- `make run` 能看到 #BP 的寄存器 dump,且内核 dump 完继续执行(打印出 `Breakpoint returned, continuing.`)。
- 连续多次 `int $3`,寄存器和 canary 值不被破坏。
- 全程没有 `sti`,也没碰任何 PIC/IRQ——那是下一个 lab 的事。
