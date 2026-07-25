---
title: 001 · ACPI 与 APIC
---

# 001 · ACPI 与 APIC

> 要做多核,第一个问题不是"怎么启动第二个核",而是更基础的:这台机器到底有几个核?它们的中断控制器在哪?BIOS 把答案写在 ACPI 表里,而多核下老式的 8259 PIC 不够用,得切到 APIC。这一章解 ACPI 表、立 APIC 驱动、把中断路由从 PIC 切到 APIC——SMP 的地基。

## 本章路线

- [01 · ACPI 与 APIC:认清硬件拓扑,把中断从老 PIC 切到 APIC](01-acpi-apic.md)
