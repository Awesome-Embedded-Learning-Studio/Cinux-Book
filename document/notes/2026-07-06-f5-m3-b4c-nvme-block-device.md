# F5-M3 批4c NvmeBlockDevice(IBlockDevice 适配)+ main.cpp 注册(并存)

**日期**:2026-07-06 · **分支**:`feat/f5-nvme-virtio`(接批4b `6c88c1c`)
**范围**:`NvmeBlockDevice`(`IBlockDevice` 适配,抄 `AHCIBlockDevice`)+ main.cpp Step 21a 注册(**并存**:生产 rootfs 仍 AHCI)+ test_nvme NvmeBlockDevice round-trip 机制测。批4c 让 NVMe 能作 `IBlockDevice` 喂 Ext2 / perf。

## 成果
- **`NvmeBlockDevice`**(`nvme_block_device.hpp/cpp`):`IBlockDevice` 适配 NVMe namespace。持单 4KB DmaBuffer(像 AHCIBlockDevice);`read_blocks`/`write_blocks` memcpy 桥接 caller buf ↔ DMA buf,调 `NvmeController::read/write_blocks`;>4KB 拒绝(单页 PRP1,PRP list 留 follow-up);`block_count`/`block_size` 从 Identify Namespace(nsze / lba_size)。
- **main.cpp Step 21a**:并存注册 NVMe 控制器(`init` + `enable` + `identify_namespace` + `create_io_queues` + `init_msi_x`)+ `NvmeBlockDevice::create`(static)。**生产 rootfs 仍 AHCI**(`init.cpp` Ext2 用 `AHCIBlockDevice`);NVMe 是独立第二盘(批5 perf 用)。
- **机制测**:test_nvme 加 `NvmeBlockDevice` round-trip(slba=2,避开批4b slba=0;write pattern → read → byte-exact)。两 leg PASS。

## 接入缝
- `IBlockDevice`(block_device.hpp):`read_blocks(block, count, void*)` / `write_blocks` / `flush` / `block_count` / `block_size`。buf 是 `void*`,实现内部 DMA 映射。
- `AHCIBlockDevice`(F1-M4)同模式:持单 4KB DmaBuffer + memcpy + >4KB 拒绝。`NvmeBlockDevice` 照抄。
- `Ext2`(ext2.hpp:55 `Ext2(IBlockDevice*)`)零改——NVMe 造 `NvmeBlockDevice` 传 Ext2 构造即可(批5 切盘)。
- main.cpp Step 21(行 237 `static AHCI ahci`)+ init.cpp:54-55(`AHCIBlockDevice::create` → Ext2)。NVMe Step 21a 插 AHCI 后、e1000 前。

## Polling 模式(延续批4a `mask_all`)
- `init_msi_x` 末尾 `mask_all`(批4a 防 test kernel LAPIC 污染)。production main.cpp 走同路径(Step 17 `switch_to_apic` 后 `init_msi_x` 仍 mask_all)。
- NVMe 跑 polling(`io_submit` 轮询 IO Cq),不发 MSI。ISR(`nvme_irq_stub`, IDT[0x41])已 install(批4a irq_handlers.cpp)但不触发。
- unmask(真异步 IRQ)留 follow-up(批5 perf 若要中断驱动;现 polling 够)。

## 验证
- 全量 `cmake --build build`:production `main.cpp` Step 21a 编译过(BUILD OK)。
- `run-kernel-test-all` 两腿 **1844/0**(`NvmeBlockDevice round-trip OK` 两腿)。
- **production boot(GUI `make run`)留用户自启验证**(memory 铁律:GUI 用户启动,Claude 只准备环境):Step 21a 应不 panic + 打 `[NVMe] NvmeBlockDevice ready (nsze=2048 lba_size=512)`。

## 下一步
- **批5**:perf NVMe vs AHCI gcc/g++ I/O 对比(基线 ~6.2s gcc / ~7.4s g++)。切 gcc rootfs 到 NVMe 盘(`NvmeBlockDevice` → Ext2)+ 加 accessor。收官 note + ROADMAP ✅。
- **follow-up**:PRP list(多 block,>4KB);NVMe Admin Flush(opcode 0x09)真 `flush`;unmask MSI-X + 真 async IRQ(production 中断驱动,若 perf 需要)。
