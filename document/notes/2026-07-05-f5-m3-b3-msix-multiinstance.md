# F5-M3 批3 NVMe — MsixController 多实例 + init_msi_x(dev_.bar[0] stale 深挖)

**日期**:2026-07-05(深挖 2026-07-06)· **分支**:`feat/f5-nvme-virtio`(commit `65694b6` 降级版 + 修复 commit)
**范围**:MsixController 多实例(默认参数,向后兼容)+ NVMe `init_msi_x` 代码 + **#PF 深挖根因 + 修复**(dev_.bar[0] stale)。

## 实现
- `msix.hpp`:`MsixController::init` 加 `table_virt=0`/`pba_virt=0` 默认参数。0 → 默认 +0x40000/+0x41000(xHCI 不改);非 0 → 传入(NVMe +0x74000/+0x75000)。
- `msix_controller.cpp`:init 用 `table_v`/`pba_v` 替代硬编码;map/unmap/rollback 全用实际 virt。
- `nvme.hpp`:加 `dev_`(PCIDevice)/`msix_cap_`/`msix_` 成员 + `init_msi_x()` + `msix_table()` accessor。
- `nvme.cpp`:`init()` 存 `dev_`;`init_msi_x()` = find_capability + init @+0x74000 + mask_all + program_vector(0, 0x41, 0) + 不 enable。

## ⭐ #PF 深挖(2026-07-06,用户要求不降级,根因找到 + 修复)
**现象**:test_nvme 调 init_msi_x 后,`run_clone_tests` 第一个 `test_clone_vm_shares_addr_space` 在 `handle_pf` 内 #PF(error code 0)→ panic,单核停 876/0,两 leg 炸。批2b(无 init_msi_x)此测试绿。

**诊断**:加 handle_pf DIAG(sentinel addr_space 期间第一次 #PF)→ `fault=0x1254 rip=clone err=2(write)`。`0x1254 = sentinel addr_space(0x1234)+0x20`:
- **clone(kCloneVm) 行 291** `child->addr_space->acquire()`(Q4e-1 DEBT-006 共享 refcount)**解引用 sentinel 0x1234 写 refcount**(offset 0x20 → 0x1254)→ #PF。
- handle_pf demand-page 分支(行 186)再解引用 `addr_space->vmas()` → 死循环 → panic。
- 批2b(跳过 init_msi_x)**0 个 PF-DIAG** → init_msi_x 使 0x1234 区 not present。

**真根因**:**`dev_.bar[0]` 是 stale 0**!`init()` 先 `read_bars`(此时 SeaBIOS 没配 → bar[0]=0),**之后**才 self-assign BAR0 寄存器=0xfeb40000,但 `PCIDevice.bar[0]` 字段没更新。`init_msi_x` 用 `dev_.bar[table_bar]`(=0)+ `table_offset` → MSI-X Table phys = **低内存(~0x2000)** → map +0x74000→低内存 + `program_vector` **写低内存 RAM** → 破坏 0x1234 区低内存映射 → clone acquire 写 0x1254 #PF。

**修法**:`init()` self-assign 后 `dev_.bar[0] = kAssignedBar0`。init_msi_x 用真实 BAR0(0xfeb40000)+ offset → MSI-X Table 在 NVMe BAR0(正确),不写低内存。

**教训**:① PCIDevice.bar[] 快照是 `read_bars` 时刻的;若之后 self-assign BAR 寄存器,务必同步更新 PCIDevice.bar[] 字段,否则后续用 dev.bar[] 的代码(init_msi_x)拿到 stale 值。② 机制测加 DIAG(条件 sentinel)精确定位第一次 #PF,5 层根因链(#PF → handle_pf 解引用 → clone acquire 写字段 → 0x1234 区被破坏 → MSI-X Table phys 错 → dev_.bar[0] stale)逐层挖通。

## 验证(真验,非降级)
两 leg 886/0 + `[NVMe] MSI-X configured (entry 0 -> vector 0x41, 65 entries)` + init_msi_x 真跑 + 机制测验 `msix_table[0].msg_addr_lower != 0`(entry 0 编程)。MsixController 多实例(@+0x74000,xHCI 默认 +0x40000 不破)+ NVMe MSI-X 配置闭环。全量编绿(xHCI 不破)+ page_fault.cpp DIAG 还原干净。

## MSI-X enable + ISR(留批4 production)
init_msi_x 不 enable(test kernel 无 IDT[0x41] ISR,vector 0x41 投递 → 跳 0 → #PF,已实证)。批4 production:注册 IDT[0x41] ISR(asm stub + handler,对齐 xHCI 批0C)→ `msix_.enable()` → Admin/IO CQ 中断驱动(vector fires 真验)。

## 下一步
批4 `NvmeBlockDevice`(IBlockDevice)+ IO SQ/CQ + Read/Write(PRP)+ main.cpp Step 21c 注册(**并存**)。**轮询模式**(沿用批2b),MSI-X enable+ISR 留批4b/follow-up。
