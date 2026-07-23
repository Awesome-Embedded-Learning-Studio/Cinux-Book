# F5-M3 批4b NVM Read/Write(单页 PRP1)+ IO queue 接通

**日期**:2026-07-06 · **分支**:`feat/f5-nvme-virtio`(接批4a `dcf4b42`)
**范围**:NVM Read(opcode 0x02)/ Write(0x01)单页 PRP1 + `io_submit` helper(IO queue 轮询)+ read/write round-trip 机制测。批4b 让 NVMe 真能读写盘。

## 成果
- **`io_submit` helper**:提交 IO SQ[qid=1] + doorbell + 轮询 IO Cq phase(镜像 `admin_submit`,改用批4a 建的 IO SQ/CQ + IO doorbell)。
- **`nvm_io` helper**:构造 NVM Read/Write 命令 + `io_submit`。失败打 `opcode/nsid/slba/nlb/status`。
- **`read_blocks`/`write_blocks`**:`nvm_io` 包装(opcode 0x02/0x01)。
- **机制测 round-trip**:`write_blocks(1, slba=0, nlb=1)` 写 pattern `0xA5^(i&0x1F)` → `read_blocks(1, 0, 1)` 读回 → byte-exact 比对。两 leg `Read/Write round-trip: 512 bytes OK`。

## NVM Read/Write 命令格式(`NvmeRwCmd`,QEMU `hw/nvme/nvme.h` 权威)
- `cdw10` = SLBA[31:0],`cdw11` = SLBA[63:32](64-bit 起始 LBA)。
- `cdw12[15:0]` = NLB-1(0-based;QEMU `nlb = le16_to_cpu(rw->nlb) + 1`)。
- `cdw12[31:16]` = control(PRINFO/FUA 等,默认 0)。
- `dptr` = PRP1(`cmd.prp1`)+ PRP2(`cmd.prp2`)。

## PRP 规则(QEMU `nvme_map_prp`,`hw/nvme/ctrl.c:876`)
- `trans_len`(第一 PRP 长度)= `page_size - (prp1 % page_size)`;PRP1 可带页内 offset。
- 数据 ≤ 1 页:**只用 PRP1**(PRP2 不看)。
- 数据 1..2 页:PRP2 是单个页地址(必须页对齐,无 offset)。
- 数据 > 2 页:PRP2 指向 PRP List(4KB 页,最多 512 PRP 条目,每条页对齐)。
- **批4b 只支持单页传输**(`nlb*lba_size ≤ 4096`,PRP1=buf、PRP2=0)。PRP list(多 block)留批4c `NvmeBlockDevice` 需要时加。

## 验证
- `run-kernel-test-all` 两腿 **1844/0**(`test_find_and_map` 两腿 PASS,`Read/Write round-trip: 512 bytes OK` 两腿)。

## 下一步
- **批4c**:`NvmeBlockDevice`(`IBlockDevice` 适配,抄 [AHCIBlockDevice](../../kernel/drivers/ahci/ahci_block_device.hpp))+ main.cpp Step 21c 注册(**并存**:NVMe 独立盘,生产仍 AHCI)+ ISR install + `switch_to_apic` + unmask MSI-X + PRP list(多 block 读)。
- **批5**:perf NVMe vs AHCI gcc/g++ I/O 对比(基线 ~6.2s gcc / ~7.4s g++)+ 收官 note + ROADMAP ✅。
