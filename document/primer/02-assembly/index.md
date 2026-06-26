---
title: x86 汇编速通(GAS)
---

# x86 汇编速通(GAS)

> 以 **GAS/AT&T 为本位**:讲清语法骨架、寻址与系统指令(CR/MSR/lgdt/iretq 的 AT&T 形态),最后给一张 AT&T↔Intel 速查表。读完能逐行读懂 `mbr.S`/`interrupts.S`/`context_switch.S`。本模块不讲 NASM——遇到 NASM 例子一律翻译成 AT&T。

- [01 · GAS 语法骨架](01-gas-syntax-skeleton.md)
- [02 · 寻址、远转移与系统指令](02-addressing-system-instr.md)
- [03 · AT&T↔Intel 速查表与 GAS 实战](03-cheatsheet-and-practice.md)
