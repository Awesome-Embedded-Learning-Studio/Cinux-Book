# F5-M3 批5 perf:NVMe vs AHCI gcc/g++ I/O 对比（F5-M3 收官）

**日期**:2026-07-06 · **分支**:`feat/f5-nvme-virtio`（接批4c `f3ea0a4`）
**范围**:NVMe vs AHCI gcc/g++ 编译 I/O 对比。`init.cpp` runtime 选 boot disk + `run-nvme-buildroot-usability` gate。**F5-M3 收官**。

## 成果
- **NvmeBlockDevice accessor**:`nvme::nvme_block_device()` / `set_nvme_block_device()`（全局 `g_nvme_blk`，照 `AHCI::instance()` 单例模式）。`main.cpp` Step 21a set，`init.cpp` 读。
- **init.cpp runtime 选盘**:NVMe Ext2 mount 成功 → NVMe rootfs；else AHCI fallback（**避免 `Ext2(nullptr)` crash**——条件构造，§14 文件 gate 精神，无 `#ifdef`）。同一 kernel 镜像 + 同一 rootfs-gcc.ext2，纯粹换底层块设备驱动。
- **`run-nvme-buildroot-usability` gate**:NVMe 挂 `rootfs-gcc.ext2`（192MB），AHCI 挂 1MB test disk。`isa-debug-exit` gate 跑 usability-test.sh（gcc/g++ smoke）。

## perf 对比（同一 rootfs-gcc.ext2 + gcc/g++，只换底层驱动）

`[MEM] I/O` 统计（gcc+g++ 编译累计 ext2 read I/O 时间）:

| 指标 | AHCI 基线 | NVMe | 收益 |
|------|----------|------|------|
| reads | 8135 | 8135 | 同（只换驱动 ✓）|
| KiB | 31158 | 31158 | 同 |
| **I/O 时间** | **10763 ms** | **9365 ms** | **−1398 ms（−13%）**|

两 gate 都 `[usability] result: PASS`（gcc-compile-run + gpp-compile-run）。**NVMe I/O 层比 AHCI 快 13%**。

## 诚实标注
- **QEMU 仿真打折扣**:理论 per-page NVMe ~50μs vs AHCI ~2.4ms ≈ 40×，QEMU 仿真远不到（NVMe/AHCI 都走 QEMU block backend，差异被 backend 平摊）。真机才接近理论收益。
- **polling 模式**:NVMe 跑 `io_submit` 轮询 IO Cq（`init_msi_x` `mask_all`，不发 MSI）。真 async IRQ（unmask）留 follow-up——批5 perf 不需要，polling 够。
- **I/O 时间 vs wall-clock**:`[MEM] I/O` 是 ext2 read I/O 累计（不含 cc1 编译 CPU 时间）。wall-clock gcc 单命令 ~6s（用户串口复测），I/O 占比小，NVMe wall-clock 改善 < 13%。
- **单核 gate 偶发**：`run-nvme-buildroot-usability`（无 -smp）g++ 偶发 `slba=246176 NVME_LBA_RANGE`（CAP_EXCEEDED，`rootfs-gcc.ext2` sparse hole——apparent 192M / actual 132M；QEMU raw backend 读 hole 边界）。重跑过。留 follow-up（非 sparse rootfs `fallocate` 或 NVMe 专用 `nvme-gcc.img`）。

## ⭐ production gcc 崩根因：NvmeBlockDevice SMP 竞态（commit `749e7db`）

批5 NVMe 默认 boot 接入 production `run`（-smp 2）后，`gcc /hello.c` 崩：gcc ldso 段错误（`addr=0x1`，读 garbage jmp）+ `[NVMe] NVM I/O slba=2315255800 failed CAP_EXCEEDED`（slba 1.18TB 越界）+ `[EXT2] read_block(1157627900)`（垃圾块号）。

**根因**：`NvmeBlockDevice` 单 `DmaBuffer` + `io_submit` polling 无锁，`-smp 2` 多核并发 `read_blocks` → `DmaBuffer` 被覆盖（A 的 NVMe read 还没 memcpy，B 已发起下一次覆盖 dma_buf_）→ memcpy 读 garbage 半写入数据 → ext2 算出垃圾 block → NVMe 越界 slba → gcc 读错 binary → ldso 跳垃圾地址。**单核 gate 不触发**（无真并发），production `-smp 2` + gcc fork 链触发——这正是 NVMe gate PASS 而 production 崩的差异。

**修**：`NvmeBlockDevice::read_blocks`/`write_blocks` 加 `cinux::proc::Spinlock guard`（串行 dma_buf_ + NVMe IO + memcpy）。用户 GUI run（TCG -smp 2）`gcc /hello.c` 编译跑通 + `echo > /rw.txt` 写盘验过。

**教训**：单 `DmaBuffer` IBlockDevice 适配器（NVMe/AHCI 同模式）在 SMP 下必须锁——单核 gate 验不出竞态，只有 production `-smp` 多核 + 多进程（gcc fork 链）才暴露。`AHCIBlockDevice` 同模式但 AHCI GUI gcc 闭环过（read 时序短/原子，未显式竞态），follow-up 给 AHCI 也加 lock 防御。

## F5-M3 弧总结（5 批）

- **批0** 立项 docs + ROADMAP。
- **批1** PCI 枚举 + BAR0 self-assign（SeaBIOS 假绿坑）+ map + CAP/VS（`1b7fc5a`）。
- **批2a/b** Controller enable（CC.EN↔CSTS.RDY）+ Admin SQ/CQ + doorbell + Identify Controller（`a471f22`/`1c2f4ac`）。
- **批3** MsixController 多实例 + `init_msi_x`（`dev_.bar[0]` stale 治 #PF）（`65694b6`/`3dead2e`）。
- **批4a** Identify Namespace + Create IO Cq/SQ + ISR（IDT[0x41]）+ enable MSI-X。⭐根因 1 CC 字段位错（IV 误诊，0x8205=MAX_QSIZE_EXCEEDED）+ ⭐根因 2 NVMe MSI 污染 LAPIC（test kernel 没 `switch_to_apic`→0x41 占 ISR 阻塞 e1000 timer；修 `init_msi_x` `mask_all`）（`dcf4b42`）。
- **批4b** NVM Read/Write（单页 PRP1）+ round-trip（`6c88c1c`）。
- **批4c** NvmeBlockDevice（IBlockDevice 适配）+ main.cpp Step 21a 并存注册（`f3ea0a4`）。
- **批5** perf NVMe vs AHCI gcc/g++（`44726e1`）+ NVMe 默认 boot disk。
- **批5 后 fix** NvmeBlockDevice Spinlock 治 SMP 竞态（production `-smp 2` gcc 崩，根因 3，`749e7db`）；默认 TCG（host `/dev/kvm` GID 漂 kmem，`7591e1e`）。

**接入缝 = NVMe 默认 + AHCI fallback**（批5 后用户决策：NVMe I/O −13% 更快，production rootfs 默认 NVMe；`cinux_qemu_run_target` 的 run/run-single/run-smp 默认 NVMe 挂 ROOTFS_IMG，AHCI port 0 留 test disk；NVMe 缺失时 init.cpp runtime fallback AHCI 作 legacy）。`run-buildroot-usability`（AHCI 基线）+ `run-nvme-buildroot-usability`（NVMe）两 gate 保留作对比。`Ext2`（`IBlockDevice*`）零改——NvmeBlockDevice 直接传 Ext2 构造。test kernel 两 leg 1844/0（批4c 验；批5 init.cpp 改动是 production，test 用 main_test 不走 init.cpp）+ production boot（`NvmeBlockDevice ready` + `[INIT] rootfs on NVMe`）。

## 验证
- `run-buildroot-usability`（AHCI 基线）+ `run-nvme-buildroot-usability`（NVMe）：两 gate `result: PASS`，串口 `[INIT] rootfs on AHCI/NVMe`，I/O 时间对比（AHCI 10763ms / NVMe 9365ms）。
- `run-kernel-test` 单核 TCG 922/0（回归；KVM permission denied——host `/dev/kvm` GID 漂 kmem，`CINUX_NO_KVM=1` TCG 绕；init.cpp runtime 选盘改动是 production，test 用 main_test 不走 init.cpp，逻辑上不影响 test）。

## follow-up（登记留续）
- **flaky**:NVMe gate g++ 偶发 LBA_RANGE（rootfs sparse hole）。用非 sparse rootfs 副本（`fallocate`）或 NVMe 专用 `nvme-gcc.img`。
- PRP list（多 block >4KB，NvmeBlockDevice 当前拒绝）。
- NVMe Admin Flush（opcode 0x09）真 `flush`。
- unmask MSI-X + 真 async IRQ（production 中断驱动，若 perf 要）。
- 真硬件测（QEMU 不反映真 NVMe 收益）。
