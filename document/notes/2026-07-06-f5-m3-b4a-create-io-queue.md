# F5-M3 批4a Identify Namespace + Create IO queues + MSI-X enable

**日期**:2026-07-06 · **分支**:`feat/f5-nvme-virtio`(接批3 `3dead2e`)
**范围**:Identify Namespace(CNS=0x00)+ Create IO Cq/SQ(qid=1)+ ISR(IDT[0x41])+ enable MSI-X。批4a 把 NVMe 从"能发 admin 命令"推进到"IO queue 就绪,准备 NVM Read/Write"。

> 前序交接 note `2026-07-06-f5-m3-b4a-io-queue-iv-handoff.md` 把 Create IO Cq 卡点误诊为 "Invalid Interrupt Vector"。本批破案:**真因是 CC.IOSQES/IOCQES 字段位错**,IV 全程没毛病。该交接 note 已删。

## 成果
- **Identify Namespace**:nsze=2048 lba_size=512(1 MB namespace)。CNS=0x00(nsid-specific),lbaf LBADS=bits[23:16](`1<<((lbaf>>16)&0xFF)`,QEMU lbaf=0x90000)。
- **admin_submit helper**:Admin SQ[tail] + doorbell + 轮询 CQ phase,返 status(SC/SCT)。Identify Controller/Namespace/Create IO Cq/SQ 共用。
- **Create IO Cq/SQ 成功**:qid=1 size=64,IO doorbell 就绪(SQ tail @ BAR0+0x1000+2*stride,CQ head @ +0x1000+3*stride)。CQE status=0。
- **ISR 落地**:`nvme_irq_handler`(IDT[0x41])+ `g_nvme_irq_count`(production 中断路径;EOI 由 ISR_IRQ stub 拥有)。
- **enable MSI-X**:entry 0/1 → vector 0x41,MC.Enable=1(CinuxOS 读 table_size=65 = QEMU 默认 `msix_qsize`)。

## ⭐ 根因 1:Create IO Cq 卡在 CC.IOSQES/IOCQES 字段位错(非 IV)

### 现象
Create IO Cq 返回 CQE `status_field=0x8205`。交接 note 据此断言 "Invalid Interrupt Vector"(SC=2 SCT=1)。

### 诊断(抓 QEMU v8.2.0 `hw/nvme/ctrl.c` `nvme_create_cq` 源码定论)
检查顺序:
```c
if (iosqes != NVME_SQES || iocqes != NVME_CQES)   // 检查 1 entry-size
    return NVME_MAX_QSIZE_EXCEEDED | NVME_DNR;     // ← 最先!
if (!cqid || cqid > conf_ioqpairs || ...)           // 检查 2 QID
    return NVME_INVALID_QID | NVME_DNR;
if (!qsize || qsize > MQES)                         // 检查 3 qsize
    return NVME_MAX_QSIZE_EXCEEDED | NVME_DNR;
... prp 对齐 ...
if (!msix_enabled(...) && vector)                   // 检查 5 IV(在后面!)
    return NVME_INVALID_IRQ_VECTOR | NVME_DNR;
if (vector >= conf_msix_qsize)                      // 检查 6 IV 范围
    return NVME_INVALID_IRQ_VECTOR | NVME_DNR;
```
status code 数值(QEMU `include/block/nvme.h` 权威):
- `NVME_MAX_QSIZE_EXCEEDED = 0x0102`(SC=2 SCT=1)
- `NVME_INVALID_IRQ_VECTOR = 0x0108`(SC=8 SCT=1)—— **完全不同的 SC**
- `NVME_DNR = 0x4000`(bit 14)

解码 `status_field=0x8205`:`(0x8205>>1) = 0x4102 = 0x0102 | 0x4000` → **`NVME_MAX_QSIZE_EXCEEDED | NVME_DNR`**,不是 IV invalid。IV 检查(5/6)在检查 1 **之后**,改 IV 永远到不了 → 交接 note 试遍 IV=0/1/0x41 + IEN=0/1 + MSI-X on/off 全 fail 是必然,与 IV 无关。

### 真因:CC 字段位
QEMU CC 宏(`include/block/nvme.h`,对齐 NVMe 1.4 spec):
- `CC_IOSQES_SHIFT = 16` → IOSQES = bits[19:16]
- `CC_IOCQES_SHIFT = 20` → IOCQES = bits[23:20]

CinuxOS 批2a 的 `kCcEnable = (6u << 24) | (4u << 16) | 1u`(注释 `IOSQES[29:24]=6, IOCQES[21:16]=4`)实际写:
- 6 进 reserved bits[27:24](浪费)
- IOSQES(bits[19:16])= 4(应 6)
- IOCQES(bits[23:20])= 0(应 4)

→ `iosqes=4 != NVME_SQES=6` → 检查 1 fail。

**Admin queue 用固定 64B/16B entry(不看 CC.IOSQES/IOCQES)**,所以 CC 位错被 Identify Controller/Namespace 全掩盖,到 Create IO Cq(第一个查 entry-size 的命令)才暴露。

### 修复
```cpp
// CC (NVMe 1.4): IOCQES[23:20]=4 (16B CQ), IOSQES[19:16]=6 (64B SQ), EN[0]=1
constexpr uint32_t kCcEnable = (4u << 20) | (6u << 16) | 1u;
```

## ⭐ 根因 2:e1000 ARP 回归——NVMe MSI 污染 LAPIC(test kernel 没 switch_to_apic)

CC 修好后,run-kernel-test-all leg1 卡在 `test_e1000::test_arp_roundtrip`(`sti+hlt` 等 LAPIC timer 0x30,永不唤醒)。对照实验(注释 `run_nvme_tests()`):两 leg 1842/0 全绿 → **NVMe 测试引入**。

### 链
1. NVMe admin completion(admin_submit polling 的 4 次:Identify Ctrl/NS、Create IO Cq/SQ)→ QEMU 发 MSI(vector 0x41)→ LAPAC IRR。
2. **test kernel 从不调 `switch_to_apic()`**(只 `g_lapic.init/enable`),`g_irq_backend = kPic` default(production `main.cpp:216` 才 switch)。
3. e1000 ARP 第一次 `sti` → 0x41 投递 → `nvme_irq_stub` → `irq_eoi_isr` → `irq_eoi` → **`PIC::send_eoi`(对 LAPAC 是 no-op)** → 0x41 留 LAPAC ISR。
4. ISR 有 0x41(优先级高)→ LAPAC Fixed IRQ 仲裁阻塞 0x30 timer(优先级低)→ e1000 ARP `sti;hlt` 永不唤醒 → 卡。

正是 `interrupts.S:264` + `irq_handlers.cpp:77` 警告的 "PIC EOI no-op → LAPAC ISR 占位 → 阻塞低优先级 IRQ" bug 模式。xHCI 也 enable MSI-X 但测试不触发 MSI(无 doorbell/event),所以不卡;NVMe admin completion 是第一个真触发 MSI 的驱动测试。

### 修复
`init_msi_x` 末尾 `mask_all`:entry masked → QEMU 不发 MSI(MSI-X spec:masked entry 记 PBA pending 不投递)→ 根本不污染 LAPAC。机制验证(MC.Enable=1 + entry programming `msg_addr != 0`)仍有效(masked entry 内容还在,只 `vector_control=1`)。production(批4c)ISR install + `switch_to_apic` 后再 unmask。

## 教训
1. **status code 别凭记忆,查权威源码**。交接 note 把 SC=2 误读成 IV invalid(实际 SC=8 才是)。`NVME_MAX_QSIZE_EXCEEDED` 在 QEMU 里复用表示 entry-size 检查(实现细节,spec 字面是 queue size)。
2. **诊断从 `nvme_create_cq` 检查顺序看**,不猜字段。IV 检查在 entry-size 之后,改 IV 治不了 entry-size 的错。
3. **admin queue 不查 CC.IOSQES/IOCQES**,CC 位错被 admin 命令全掩盖。Create IO queue 是第一个暴露点——给"机制测过了"敲警钟。
4. **test kernel 没 `switch_to_apic`**(`g_irq_backend=kPic`),`irq_eoi` 走 PIC no-op。任何 unmasked MSI(测试期间真触发的)都会污染 LAPAC ISR,阻塞后续 LAPAC timer 测试(e1000 ARP)。驱动测试要么 mask entry(不发 MSI),要么测试结束显式 `g_lapic.eoi()`。
5. QEMU nvme 默认 `msix_qsize=65`、`max_ioqpairs=64`,IV/QID 资源充足,不存在资源不足类根因。

## 验证
- run-kernel-test-all 两腿 **1844/0**(NVMe `test_find_and_map` 两腿 PASS,Create IO Cq/SQ status=0;e1000 ARP 两腿 PASS)。

## 下一步
- **批4b**:NVM Read(opcode 0x02)/ Write(0x01)PRP SGL(nsid + slba + nlb + PRP1/PRP2)+ IO Cq 轮询/中断。机制测:read/write round-trip。
- **批4c**:`NvmeBlockDevice`(`IBlockDevice` 适配,抄 [AHCIBlockDevice](../../kernel/drivers/ahci/ahci_block_device.hpp))+ main.cpp Step 21c 注册(**并存**:NVMe 独立盘,生产仍 AHCI)+ ISR install + `switch_to_apic` + unmask MSI-X。
- **批5**:perf NVMe vs AHCI gcc/g++ I/O 对比(基线 ~6.2s gcc / ~7.4s g++)+ 收官 note + ROADMAP ✅。
