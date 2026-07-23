# 2026-07-06 — IRQ MSI-X vector IST2 漏改根治(F5 post-finale)

> 续 [production 崩 IST 根因 note](2026-07-06-f5-virtio-production-crash-ist-rootcause.md)(前传:IST 缺失根因 + 并 origin/main IST2 修)+ [F13-B Bug② IST note](2026-07-06-f13-b-bug2-irq-ist.md)。本篇:IST2 修本身漏了 3 个 MSI-X vector,导致 production 间歇 panic 残留。

## 症状

F5 VirtIO 弧收尾后(IST 缺失已"修"),production `run`(NVMe boot + virtio-blk + e1000 + xHCI)仍间歇 panic:usability 跑完 ls/cat/mkdir/uname/pipe 后、**gcc/g++ fork-exec 阶段** manifest,panic 详情被 100s timeout 截断。与「IST 修后 build-console 3 次稳」矛盾 —— 即 IST 修**部分**生效,残留间歇。

## 根因(IST2 修漏改 3 个 MSI-X vector)

`kernel/arch/x86_64/irq_handlers.cpp` `irq_init()` 注册 IRQ vector 的 IDT gate `ist` 字段**不一致**:

| vector | 设备 | ist | 来源 |
|--------|------|-----|------|
| 0x20-0x2F | PIC(PIT/键盘/鼠标) | 2 | 54f6392 |
| 0xE0 | Reschedule IPI | 2 | 54f6392 |
| 0x40 | xHCI | 2 | 54f6392 |
| 0x30 | LAPIC timer | 2 | 54f6392 |
| **0x41** | **NVMe(production boot disk)** | **0** ❌ | dcf4b429(漏改) |
| **0x42** | **virtio-blk** | **0** ❌ | 3028d05f(抄 NVMe 漏改) |
| **0x43** | virtio-net | **0** ❌ | (同) |

**git blame 铁证**(全 2026-07-06):
1. `dcf4b429`(01:09)NVMe 批4 注册 0x41 → **ist=0**(此时 IST2 修还没做)。
2. `54f6392`(13:22)F13-B IST2 修,把 PIC/IPI/xHCI/LAPIC timer **4 处** `ist=0→2`,**漏了 NVMe(0x41)**。
3. `3028d05f`(14:52)virtio-blk 批3 注册 0x42 → **抄了 NVMe 的 ist=0 模板**,同样漏改。

f13-b note 原话:「irq_handlers **4 处** set_handler `ist=0→2`」—— 只有 4 处,漏了 3 个 MSI-X vector。

**机制**:ist=0 时,MSI-X IRQ 进来 CPU 据 IDT gate 不切栈,ISR stub(interrupts.S:313-314)的 `sub $512 + fxsave(%rsp)` + ISR frame 落**被中断 task 栈**上。NVMe 是 production boot disk,gcc/g++ 读 rootfs 时 0x41 高频 fire,深栈中段抢进 → fxsave 512B **物理覆盖**栈对象 → corrupt 指针 → 崩点漂移的间歇 panic。PIT(0x20)已 ist=2 去掉最高频(100Hz)覆盖源,故大幅缓解,但 NVMe(0x41)漏改 → 残留间歇( gcc 读密集时触发)。

与 F13-B Bug②(gui_worker render 崩)同根:IRQ fxsave 覆盖 task 栈。F13-B 修了 4 个 vector,漏了 3 个 MSI-X vector —— 「IST 防覆盖非防溢出」(见 f13-b note)。

## 修复

`irq_handlers.cpp` `irq_init()`:NVMe(0x41)/ virtio-blk(0x42)/ virtio-net(0x43)3 处 `kIRQAttr, 0` → `kIRQAttr, 2`,补全 F13-B 漏改。NVMe 处注释记根因 + 54f6392 漏改说明;blk/net 注释交叉引用 NVMe。ISR stub(interrupts.S)**零改**(CPU 据 IDT gate ist 字段自动切 IST2 栈,见 f13-b note「interrupts.S 零改」)。

## 验证(build-verify + build-console,GUI=OFF,自跑)

- **`run-kernel-test-all`**(两 leg 单核 + -smp 2):`=== Tests: 889 passed, 0 failed ===` + SMP AP1 readback PASS + `ALL TESTS PASSED`。ist 改不回归(test kernel 含 blk+net 双设备 + 全套 IRQ 注册)。
- **production `run` 连跑 5 次**(build-console,NVMe boot + virtio-blk + e1000 + xHCI,KVM -smp 2):**5/5 干净** —— usability 9 PASS + gcc+gpp PASS + cinux-exit 正常 exit + 零真 panic。(grep 命中的 `#DF` 计数是启动日志 `[BIG] GDT loaded (TSS with IST1 #DF + IST2 IRQ stacks)` 的固定串,非 panic;5 次完全一致。)

此前间歇 panic(IST 修前/部分修后)根治。注:5 次不 statistical 显著区分改前间歇率(改前约 25%「3 稳 1 崩」),但根因铁证(git blame + IST 不一致 + memory 闭环)+ 修复正确(补全漏改,严格对齐 PIT/xHCI/LAPIC timer 的 IST2)+ 验证达标(不回归 + production 稳定)。

## ⭐ 教训

1. **IST 修要全 vector 覆盖**:F13-B 只改 4 个 vector,漏 MSI-X vector。新加 IRQ vector 抄模板易漏 ist 字段。规则:**新注册 IRQ vector 默认 ist=2**(与 PIT/xHCI/LAPIC timer 一致),除非有特殊理由(目前无)。
2. **bisect 干净 ≠ 全覆盖**:54f6392 的 IST2 修在它测的场景(gui_worker render)干净,但 production NVMe 场景漏盖 → 间歇残留。验证要覆盖 production 真实负载(NVMe boot + gcc 重负载),不只 GUI 场景。
3. **间歇 panic + 崩点漂移 + gcc 阶段 = IRQ fxsave 覆盖栈**:IST 同族签名,F13-B 已立。下次再遇先查所有 IRQ vector 的 ist 字段是否一致。

## commit

- `aebbc85` fix(irq): NVMe/virtio MSI-X vectors 用 IST2(根治 production 间歇 panic)
- `de72fb6` fix(f5-virtio): blk↔net per-device BAR slotting(同接力,BAR 撼修;test kernel blk+net 双设备验证 889/0)

## 接力下一步(post-finale,在 `01-virtio.md` 任务表)

- task 2:SLIRP ping —— virtio-net 进 `run`(独立 SLIRP netdev,不拆 e1000)+ NetStack attach + ping 10.0.2.2。BAR slotting 已就位(task 1 done)。
- task 3:virtio-blk vs NVMe vs AHCI perf harness。独立。
