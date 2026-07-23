# F5-M3 批1 NVMe — PCI 枚举 + BAR0 自分配 + CAP/VS 机制测

**日期**:2026-07-05 · **分支**:`feat/f5-nvme-virtio`(commit `b5b866f`,从干净 main `bef86b0`)
**范围**:PCI 枚举 NVMe + BAR0 映射 @KMEM_MMIO+0x70000 + QEMU `-device nvme` 独立盘 + 机制测读 CAP/VS 证映射真生效。

## 实现
- `pci/pci_config.hpp` + `NVME_SUBCLASS=0x08`;`pci.hpp` + `is_nvme_device()`(class 0x01/sub 0x08,prog_if 不强制)+ `find_nvme()`(走 `find_device` 统一框架,log_bar=0)。
- `drivers/nvme/nvme.{hpp,cpp}`(新):`NvmeController` 类 + `NvmeRegs`(NVMe 1.x 寄存器 0x00-0x34),`init()` 抄 xHCI(PCI COMMAND enable → `g_vmm.map` BAR0 @KMEM_MMIO+0x70000 → 读 CAP/VS)。批1 只读到 CAP/VS,不做 CC.EN(留批2)。
- `cmake/qemu.cmake` + `NVME_TEST_IMAGE`(1 MB raw,复用 `create_ahci_test_disk.sh`)+ `-device nvme,drive=nvme-disk,serial=nvme0` 加进 `QEMU_TEST_EXTRA_FLAGS`(两 leg 都带)+ factory/`run-kernel-test-all` DEPENDS 加 `${NVME_TEST_IMAGE}`。
- `test/test_nvme.cpp`(新)+ `run_nvme_tests()`:`test_find_and_map`(find_nvme + init + 断言 MQES>0 + MJR>=1,skip if no nvme)。

## ⭐ 踩坑:SeaBIOS 不配 QEMU nvme BAR0 → 假绿(头号教训)
**现象**:首跑 `[PCI] NVMe found: 00:04.0 BAR0=0x0`(SeaBIOS 配 AHCI/e1000 但跳过 QEMU nvme),`init` map phys 0x0(低 RAM)读 CAP/VS 返垃圾:`MQES=65363 / VS=0xf000e2c3` —— 恰好满足 ">0" 断言 = **假绿**(两 leg `[PASS]` 但读的根本不是 NVMe 寄存器)。

**根因**:CinuxOS PCI 层(`pci.cpp::read_bars`)只读 BAR 不分配,依赖 SeaBIOS;SeaBIOS 配 AHCI(BAR5=0xfebf1000)/e1000 但**不配 QEMU nvme**(原因未深究,可能 SeaBIOS 的 nvme boot driver 提前接管或跳过 BAR 配置)。

**修**:NVMe init 加 BAR self-assign(标准 PCI BAR 分配流程,hobby-OS 合理做法):
1. probe size:写 `BAR0=BAR1=0xFFFFFFFF`,读回 `~(probe & BAR_ADDR_MASK_32)+1` = **16384** = 16 KB BAR0。
2. 分配固定槽 `0xfeb40000`(16 KB 对齐,避 AHCI BAR5 @0xfebf1000)。
3. **64-bit BAR 必须清 BAR1**(写 `BAR1=0`):否则地址 = BAR1<<32 | 0xfeb40000(高地址),map 0xfeb40000 读不到设备 —— 这是首修读到 0 的直接原因。

修后:`rb0=0xfeb40004`(0x4 = 64-bit memory BAR type bits,写入生效)/ `rb1=0x0` → `CAP.MQES=2047`(0x7FF,max 2048 entries)/ `VS=0x10400`(MJR=1.MNR=4 = NVMe 1.4)/ `DSTRD=15`。

**教训**:① 机制测断言要能区分真值 vs 垃圾(MQES 合理范围 + MJR>=1,不只 >0);② memory `dont-drop-blocked-tests-diagnose` + `mechanism-test-and-debug-not-qemu` 奏效——首版假绿被"886 passed 1 failed"(MQES=0 撞断言)暴露,二修后真绿。通用 PCI BAR 分配器(对所有 BAR=0 设备做分配)留 PCI 框架 follow-up。

## 验证
- `cmake --build build --target big_kernel_test` 编通(kallsyms 18074)。
- `timeout 180 cmake --build build --target run-kernel-test-all` 两 leg **886/0**(单核 + -smp 2),`[NVMe] mechanism: MQES=2047 VS=0x10400`,`[PASS] test_nvme::test_find_and_map`,两 leg `[TEST] ALL TESTS PASSED (exit code 0)`。
- ⚠️ **临时 VNC 切 `-vnc :7` 用 sed 反向还原(`s/-vnc :7/-vnc :0/g`),别用 `git checkout cmake/qemu.cmake`** —— 会误杀未 commit 的 nvme 改动(踩过一次,重落 4 处 Edit)。memory `verify-vnc-port-collision-multi-session`。

## 下一步
批2 Controller init(CC.EN↔CSTS.RDY 握手)+ Admin SQ/CQ + doorbell(stride = 4<<DSTRD,DSTRD=15 待批2 验证)+ Identify Controller(轮询 CQ,无中断)。
