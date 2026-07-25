---
title: Lab 001 · ACPI 与 APIC 验证
---

# Lab 001 · ACPI 与 APIC 验证

> 对应 `document/book/15-smp/001/`。验证档 **B 档**(启动日志见 APIC 切换 + PIT 走时)。验证靠构建 + 测试 + grep + 启动日志。

## 目标

确认四件事:

1. ACPI 表解析在(RSDP 发现 + RSDT 遍历 + MADT 解);
2. LAPIC + IOAPIC 驱动在,APIC MMIO 用 FLAG_PCD(禁缓存);
3. PIC→APIC 切换(`irq_backend`)在;
4. 启动日志见 `[APIC] switched` + 切后 PIT(IRQ0)走时正确。

## 步骤

### 1. 构建 + 测试

```bash
git checkout 001_smp_acpi_apic 2>/dev/null || git checkout 001_*
cmake -B build -S . && cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
cmake --build build --target run-kernel-test
```

### 2. ACPI + APIC 在位

```bash
ls kernel/drivers/acpi/ kernel/drivers/apic/
grep -rn 'find_rsdp\|MADT\|switch_to_apic\|set_redirect' kernel/drivers/ kernel/arch/x86_64/irq_backend.cpp | head
grep -rn 'FLAG_PCD' kernel/drivers/apic/ kernel/arch/x86_64/ | head
```

**思考**:为什么 APIC MMIO 要 `FLAG_PCD` 而 ACPI 表用 direct-map 就行?——见章节:ACPI 表是普通 RAM(cache-enabled direct-map 正确);APIC 寄存器是设备 MMIO,必须禁缓存(FLAG_PCD),否则读到缓存旧值。**为什么表遍历主路径是 RSDT 不是 XSDT?**——QEMU `pc` 默认 ACPI 1.0(revision 0),只有 32 位 rsdt_address,无 XSDT。

### 3.(B 档)启动日志看 APIC 切换

```bash
cmake --build build --target run 2>&1 | grep -aE 'ACPI|APIC' | head
# 期望:[ACPI] RSDP found / [ACPI] N CPU, LAPIC ..., IOAPIC ... / [APIC] switched
```

切到 APIC 后,PIT(IRQ0)走时仍正确(中断经 IOAPIC→LAPIC 链路)——这就是中断后端切成功的证据。**`grep -a`**(QEMU 串口混 ANSI escape,默认 grep 静默)。

## 验收清单

- [ ] 构建 `build=0`,测试全绿(ACPI + APIC 测)。
- [ ] ACPI(rsdp/sdt/madt)+ APIC(local_apic/io_apic)+ irq_backend 都在。
- [ ] APIC MMIO 用 FLAG_PCD。
- [ ] 启动日志见 `[APIC] switched`,切后 PIT 走时对。

## 别做这些

- **别**用 direct-map(cache-enabled)访问 APIC MMIO——要 FLAG_PCD 禁缓存。
- **别**假设 QEMU 给 ACPI 2.0/XSDT——默认是 1.0 RSDT(32 位),XSDT 只作可选分支。
- **别**查 QEMU 串口日志不加 `-a`——ANSI escape 让 grep 静默。
