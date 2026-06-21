---
title: 参考 · 子系统速查
---

# 参考 · 子系统速查

> 查阅层。跨章节的子系统速查表,**不按 tag 组织**,给「读到某章、想快速查这个子系统的接口 / 寄存器 / 位常量 / 边界」用。每篇含子系统地图、接口与常量表、约束与边界(诚实标注半成品)、验证入口、源码索引、权威依据。

## 现有篇目

- [存储:PCI 枚举与 AHCI 扇区读写](storage-ahci-pci.md) — PCI 配置空间、AHCI 寄存器、命令提交、DMA
- [中断与异常:IDT、PIC、PIT、ISR 栈帧](interrupts-idt-pic-pit.md) — 异常向量表、门描述符、EOI 规则、栈对齐
- [内存:PMM、VMM、内核堆、地址空间](memory-pmm-vmm-heap.md) — 内存布局区段、页表 flag、堆上限、direct map
- [进程:上下文切换、调度器、同步原语](process-context-scheduler.md) — CpuContext 布局、context_switch、调度、Spinlock/Mutex
- [文件系统:VFS、ramdisk、ext2、文件描述符](filesystem-vfs-ext2.md) — VFS 三层抽象、FDTable、USTAR、ext2、syscall 号

实现细节回查各 [主书](/book/) 章节;真实排错故事见 [调试笔记](/debug-notes/)。
