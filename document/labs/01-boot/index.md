---
title: 01 · 引导卷实验
---

# 01 · 引导卷实验

> 从 512 字节的引导扇区出发:实模式、GDT、保护模式、长模式,直到把最小内核从磁盘加载进内存。本卷实验对应 [第 01 卷正文](/book/01-boot/)。

## 实验列表

- [lab-001 实模式与 BIOS 服务](lab-001-real-mode.md)
- [lab-002 GDT 与保护模式](lab-002-gdt-protected.md)
- [lab-003 进入长模式](lab-003-long-mode.md)
- [lab-004 从磁盘加载最小内核](lab-004-load-mini-kernel.md)

## 本章检查点

读完正文与实验后,用下面的检查点确认理解到位。进度自动保存在浏览器本地。

<CheckpointList
  heading="引导卷 · 理解检查"
  :items="[
    { src: '01-boot/boot-sector-signature', title: '引导扇区的硬性约定' },
    { src: '01-boot/real-mode-width', title: '实模式的寄存器宽度' },
    { src: '01-boot/gdt-load-order', title: '保护模式三步曲的顺序' },
  ]"
/>
