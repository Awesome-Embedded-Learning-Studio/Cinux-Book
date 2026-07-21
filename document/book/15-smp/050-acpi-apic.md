---
title: 050 · ACPI 与 APIC
---

# 050 · ACPI 与 APIC:SMP 多核弧开篇,先认清硬件拓扑

> 进 **F4 SMP 多核弧**——v1.0.0 最大的差异化弧。但在让第二个核跑起来之前,得先搞清楚:**有几个核、它们的中断控制器在哪、怎么从老的 8259 PIC 切到 APIC**。这些信息全在 BIOS 提供的 **ACPI 表**里。050 解 ACPI 表(RSDP/RSDT/MADT)+ 立 LAPIC/IOAPIC 驱动 + 做 PIC→APIC 切换。XL 档:这是 SMP 地基,概念密度高。B 档为主(启动日志可见 APIC 切换 + PIT 走时)。

## 这章咱们要点亮什么

1. **ACPI 表解析**:从 RSDP 找到 RSDT,遍历 SDT,解出 MADT(多核 CPU 列表 + LAPIC/IOAPIC 地址)。
2. **LAPIC + IOAPIC 驱动**:xAPIC MMIO 寄存器,LAPIC 本地中断、IOAPIC 重定向外部中断。
3. **PIC→APIC 切换**(`irq_backend` 抽象):mask 老 PIC、enable LAPIC、IOAPIC 重定向 ISA 中断(IRQ0/1/12 照 ACPI override)。

## ACPI:硬件拓扑的说明书

ACPI 表是 BIOS 留给 OS 的"硬件说明书"。SMP 要的几样全在里面:有几个 CPU、LAPIC 的 MMIO 基址、IOAPIC 的基址、ISA 中断怎么 override(IRQ0→GSI2 之类)。解析链:

```
RSDP(根指针) → RSDT(表目录,32 位指针数组) → 各 SDT(MADT/FADT/HPET...)
                                                ↑ MADT 里有 CPU 列表 + APIC 地址
```

`kernel/drivers/acpi/`:

- `rsdp.cpp`:**RSDP 发现**。扫两个区域——EBDA(物理 0x40E 段指针 <<4 的前 1KB)和 BIOS ROM(0xE0000–0xFFFFF),**16 字节对齐步长**(ACPI 要求)。找到 "RSD PTR " 签名后校验 checksum(ACPI 1.0 校验前 20 字节,2.0+ 额外校验整个 length)。
- `sdt.cpp`:SDT 头校验 + RSDT 遍历(32 位指针数组,每个指向一个 SDT)。
- `madt.cpp`:解 MADT(多 APIC 描述表)——里面的中断控制器结构体条目(LAPIC 条目 = 每个 CPU 一个、IOAPIC 条目、Interrupt Source Override 条目)。CPU 列表 + APIC 地址从这里来。

物理地址经 **direct-map** 访问(`phys_to_virt = DIRECT_MAP_BASE + phys`),因为 ACPI 表是普通 RAM。**注意和 APIC MMIO 区分**——APIC 寄存器是 MMIO,得单独 `VMM.map + FLAG_PCD`(禁缓存),不能用 direct-map。

### GOTCHA:QEMU 默认 ACPI 1.0

实测:**QEMU `pc` 机型默认给 ACPI 1.0 的 RSDP(revision 0)**,只有 32 位 `rsdt_address`,**没有 XSDT**(64 位)。所以表遍历的主路径必须是 **RSDT(32 位)**,XSDT 只作 revision≥2 的可选分支。propose 阶段假设的"QEMU 给 ACPI 2.0"是错的——实测暴露后改的主路径。

> 还有查日志的小坑:QEMU 串口输出混 ANSI escape,`file` 报"with escape sequences",`grep` 默认判二进制**静默不输出**(连 "Binary file matches" 都不说)。查 QEMU 串口日志一律 `grep -a` 强制文本。

## LAPIC + IOAPIC

ACPI 告诉了地址,接着立驱动(`kernel/drivers/apic/`):

- **`local_apic`**:每个 CPU 一个,MMIO 寄存器(默认基址 0xFEE00000)。管本地中断(定时器、IPI)、EOI。xAPIC 模式(早期)/ x2APIC(MSR,后面)。
- **`io_apic`**:系统级,收外部设备中断(键盘、PCI 等),按重定向表分发给某 CPU 的 LAPIC。`set_redirect` 配置一条 GSI→CPU+vector 的映射。

### GOTCHA:APIC MMIO 要 FLAG_PCD

APIC 寄存器是 MMIO 设备内存,**必须禁缓存**(FLAG_PCD,Page Cache Disable)。direct-map 是 cache-enabled 的,拿它访问 APIC MMIO 会读到缓存的旧值、行为错乱。所以 APIC MMIO 区单独 `VMM.map(addr, FLAG_PCD | FLAG_PRESENT | ...)`。

## PIC→APIC 切换(`irq_backend`)

有了 LAPIC/IOAPIC,就要把中断后端从老的 8259 **PIC** 切到 **APIC**。这一弧立一个 `irq_backend` 抽象(`kernel/arch/x86_64/irq_backend.{hpp,cpp}`):

- **mask 老 PIC**:8259 PIC 屏蔽掉,中断不再走它;
- **enable LAPIC**:每个 CPU 的 LAPIC 软件使能;
- **IOAPIC 重定向**:把 ISA 中断(IRQ0 时钟、IRQ1 键盘、IRQ12 鼠标)经 IOAPIC 重定向到 BSP(bootstrap CPU)的 LAPIC,**照 ACPI 的 Interrupt Source Override**(比如 IRQ0 实际路由到 GSI2)。

真机日志能看到 `[ACPI] 1 CPU, LAPIC 0xFEE00000, IOAPIC 0xFEC00000, 5 IRQ override, pcat=1` 然后 `[APIC] switched`——切到 APIC 后,**PIT 时钟(IRQ0)经 APIC 路由仍能触发**,证明中断链路通了。

## 验证

```bash
ls kernel/drivers/acpi/ kernel/drivers/apic/
grep -rn 'find_rsdp\|class ACPIInfo\|MADT\|switch_to_apic\|set_redirect' kernel/drivers/acpi/ kernel/drivers/apic/ kernel/arch/x86_64/irq_backend.cpp | head
grep -n 'FLAG_PCD\|pcd' kernel/drivers/apic/ kernel/arch/x86_64/ 2>/dev/null | head
```

构建 + 内核测试(这一弧 run-kernel-test 从 049 涨一截,ACPI 9 测 + APIC):

```bash
cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
cmake --build build --target run-kernel-test
```

B 档端到端:启动日志里 `[APIC] switched` + 切到 APIC 后 PIT(IRQ0)走时正确(中断经 IOAPIC→LAPIC 链路通了)。真多核(-smp 2)要等下一章 051(per-CPU + AP boot)才看得到第二个核。

## 小结与下一站

SMP 地基立了:知道有几个核、APIC 在哪、中断切到了 APIC。但此刻还是**单核**跑(BSP),LAPIC/IOAPIC 只是"通了路"。

下一站 **051** 是 SMP 最难的一步:**per-CPU 架构**(GS/swapgs/per-CPU GDT/TSS)+ **AP boot trampoline**(把第二个核从实模式拉到长模式、跑起来)。
