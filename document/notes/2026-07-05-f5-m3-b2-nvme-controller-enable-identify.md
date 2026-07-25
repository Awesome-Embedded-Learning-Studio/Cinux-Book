# F5-M3 批2 NVMe — Controller enable + Admin SQ/CQ + doorbell + Identify

**日期**:2026-07-05 · **分支**:`feat/f5-nvme-virtio`(批2a `a471f22` + 批2b `1c2f4ac`)
**范围**:CC.EN↔CSTS.RDY 握手 + Admin SQ/CQ 配置 + doorbell + Identify Controller(轮询 CQ)。

## 批2a `enable()`
- disable(CC.EN=0 → 等 CSTS.RDY=0)→ alloc Admin SQ(64×64 B=4 KB)+ CQ(64×16 B=1 KB,4 KB 页)→ 配 AQA(ASQS=ACQS=63,0-based)+ ASQ/ACQ(phys lo/hi)→ CC.EN=1(MPS=0/CSS=0/AMS=0/IOCQES=4 16 B/IOSQES=6 64 B)→ 等 CSTS.RDY=1。
- Admin SQ/CQ 用 DmaPool RAII(`admin_sq_buf_`/`admin_cq_buf_` move-only;NvmeController 持 DmaBuffer 成员 → move-only,对齐 xHCI)。
- 轮询无 IRQ:`kReadyIters=1e6` spin 等 RDY(批3 接 MSI-X 后可改 IRQ)。

## 批2b doorbell + `identify_controller()`
- doorbell stride = 4 << DSTRD。Admin SQ tail @ BAR0+0x1000,CQ head @ BAR0+0x1000+stride。
- Identify 流程:建命令(op 0x06 / CNS=0x01 / PRP1=4 KB DMA buf)入 Admin SQ[tail] → tail++(回绕)→ 写 SQ tail doorbell → 轮询 CQ[head].status bit0(phase)翻转 → status>>1 ==0 拿 controller data(VID/SSVID/SN/MN)。
- CQ phase 跟踪:`cq_phase_` 初始 1(NVMe CQ 规定);CQE.status bit0 == cq_phase_ 表新完成;ring 回绕(head=0)时 cq_phase_ ^= 1。

## ⭐ DSTRD 解码修正(批1 显示 bug)
批1/2a 显示 "DSTRD=15"(误用 `(cap_lo>>24)&0xF`)。CAP lo=0x0f0107ff 真值解码:
- MQES=bits[15:0]=0x7FF=2047
- CQR=bit16=1
- **TO=bits[27:23]=30**(QEMU;bits[27:24] 高半字节 = 0xF 被误读为 DSTRD)
- **DSTRD=bits[31:28]=0**(NVMe 1.4 编码)

批2b 改 `(cap_lo>>28)&0xF` → DSTRD=0 → stride=4(标准)。教训:NVMe CAP 字段位宽对 spec 版本敏感,误把 TO 高位当 DSTRD。doorbell offset 若按错 DSTRD=15(stride=128 KB)会超 BAR0 16 KB map。

## 验证
两 leg 886/0 + `[NVMe] enabled (admin queue=64 RDY=1 doorbell stride=4)` + `[NVMe] Identify: VID=0x1b36 SSVID=0x1af4 SN=nvme0 MN=QEMU NVMe Ctrl`。Admin queue + doorbell + Identify 全通。

## 下一步
批3 MSI-X 多实例(`MsixController::init` 加 base 参数,避碰 xHCI +0x40000,NVMe MSI-X Table @+0x74000/PBA @+0x75000)+ IO CQ 中断绑 IDT(轮询 → 中断)。
