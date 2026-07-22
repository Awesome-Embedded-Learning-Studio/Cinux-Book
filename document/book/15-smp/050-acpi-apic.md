---
title: 050 · ACPI 与 APIC
---

# 050 · 认清硬件拓扑,把中断从老 PIC 切到 APIC

> 要做多核,第一个问题不是"怎么启动第二个核",而是更基础的:**这台机器到底有几个核?它们的中断控制器在哪?** BIOS 不会口头告诉你——它把答案写在一张叫 **ACPI 表**的数据结构里。而多核下,老式的 8259 PIC(单核时代的中断控制器)不够用了,得切到 **APIC**(每核一个本地控制器 + 一个系统级分发器)。这一章解 ACPI 表、立 APIC 驱动、把中断路由从 PIC 切到 APIC。这是 SMP 的地基——此刻仍是单核跑,但中断链路已经为多核铺好。

## ACPI:BIOS 留的硬件说明书

ACPI 表是 BIOS 留给操作系统的"硬件说明书"。SMP 要的几样全在里面:有几个 CPU、每个 CPU 的本地中断控制器(LAPIC)地址、系统中断分发器(IOAPIC)地址、传统的 ISA 中断怎么路由(键盘、时钟这些)。

解析链是一路找下去(`kernel/drivers/acpi/`):

```
RSDP(根指针)→ RSDT(表目录)→ 各 SDT(MADT/FADT/HPET…)
                                  ↑ MADT 里有 CPU 列表 + APIC 地址
```

- **RSDP 怎么找**:在物理内存里扫两个区域——EBDA(扩展 BIOS 数据区)和 BIOS ROM 区,按 16 字节对齐步长找 "RSD PTR " 签名。找到后校验 checksum。
- **RSDT**:一张指向各 SDT 的指针表。遍历它找到 MADT。
- **MADT**:解出 CPU 列表(每个 LAPIC 条目一个 CPU)、IOAPIC 地址、中断源覆盖(比如时钟 IRQ0 实际路由到哪个全局中断号)。

物理地址经 direct-map 访问(ACPI 表是普通内存)。**注意和 APIC 的 MMIO 区分**——APIC 寄存器是设备内存,得禁缓存,不能用 direct-map。

### 一个实测发现:QEMU 默认给的是 ACPI 1.0

实测下来,QEMU 的默认机型给的是 **ACPI 1.0** 的 RSDP——它只有 32 位的 `rsdt_address`,**没有 64 位的 XSDT**。所以表遍历的主路径必须是 **RSDT(32 位指针)**,XSDT 只作为可选分支。这纠正了"QEMU 提供 ACPI 2.0"的想当然假设——以实测为准。

## LAPIC 与 IOAPIC

ACPI 告诉了地址,接着立驱动(`kernel/drivers/apic/`):

- **LAPIC(本地 APIC)**:每个 CPU 一个,是一块 MMIO 寄存器(默认基址 `0xFEE00000`)。管本 CPU 的本地中断(定时器、核间中断 IPI)、中断结束(EOI);
- **IOAPIC(输入输出 APIC)**:系统级,收外部设备中断(键盘、PCI 等),按重定向表把中断分发给某个 CPU 的 LAPIC。

### APIC 的 MMIO 要禁缓存

APIC 寄存器是**设备内存(MMIO)**,访问它必须**禁缓存**(页表里的 PCD 位,Page Cache Disable)。direct-map 是允许缓存的,拿它访问 APIC 会读到缓存的旧值、行为错乱。所以 APIC 的 MMIO 区单独映射,带上 PCD 位。

## 从老 PIC 切到 APIC

有了 LAPIC/IOAPIC,把中断后端从老的 8259 **PIC** 切到 **APIC**。这一章立一个中断后端抽象(`kernel/arch/x86_64/irq_backend`):

- **屏蔽老 PIC**:8259 屏蔽掉,中断不再走它;
- **使能 LAPIC**:每个 CPU 的 LAPIC 软件使能;
- **IOAPIC 重定向**:把传统 ISA 中断(时钟 IRQ0、键盘 IRQ1、鼠标 IRQ12)经 IOAPIC 重定向到 bootstrap CPU 的 LAPIC,而且**照 ACPI 的中断源覆盖**来(比如 IRQ0 实际路由到全局中断 2)。

切完之后,启动日志能看到 APIC 已切换,而且**时钟中断(IRQ0)经 APIC 路由仍能正常触发**——这就证明中断链路通了。真多核(第二个核上线)是下一章的事;这一章把"单核 + APIC 中断"铺到位。

## 验证

```bash
ls kernel/drivers/acpi/ kernel/drivers/apic/
grep -rn 'find_rsdp\|MADT\|switch_to_apic\|set_redirect' kernel/drivers/ kernel/arch/x86_64/irq_backend.cpp | head
grep -rn 'FLAG_PCD' kernel/drivers/apic/ kernel/arch/x86_64/ | head
```

构建:

```bash
cmake -B build -S . && cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
```

端到端:启动日志里能看到 ACPI 解析出的拓扑(几个 CPU、LAPIC/IOAPIC 地址)、APIC 已切换,而且切换后时钟中断走时正确(说明中断经 IOAPIC→LAPIC 的链路通了)。**真双核(`-smp 2`)要等下一章**——这一章结束时仍是单核,但中断后端已经为多核准备好了。

> 小坑:QEMU 的串口输出混着 ANSI 转义序列,`grep` 默认会把它当二进制、静默不输出。查启动日志一律加 `-a` 强制按文本。
