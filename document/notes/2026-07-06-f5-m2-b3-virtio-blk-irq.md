# F5-M2 批3:virtio-blk MSI-X 真中断路径

> 2026-07-06。F5-M2/M7 VirtIO 弧批3。virtio-blk MSI-X 真异步中断路径就位
> (production unmask,**不像 NVMe mask_all polling 留 follow-up**)。test kernel
> APIC 限制不验真触发(console gate 验 ISR install + round-trip 不破);production GUI 验。

## 新增

- `virtio.hpp/cpp`:`init_msi_x(vector)` —— MsixController 第 3 实例(Table @+0x84000 /
  PBA @+0x85000,避撞 xHCI +0x40000 / NVMe +0x74000),entry 0 → @p vector,
  `program_vector` 自带 unmask,**不 mask_all 回去**。`setup_queue` 绑
  `queue_msix_vector=0`(queue 完成投递到 entry 0)。
- `virtio_blk.cpp`:`virtio_blk_irq_handler`(extern C,计数 `g_virtio_blk_irq_count`,
  不 inline schedule——避 sti-in-syscall #DF,见 [[sys-ping-df-sti-in-syscall]])。
- `interrupts.S`:`ISR_IRQ virtio_blk_irq_stub, virtio_blk_irq_handler, 0`。
- `irq_handlers.cpp`:IDT[0x42] 注册(boot 时,AP 共享 IDT)。
- `main.cpp` Step 21a2:production 调 `init_msi_x(0x42)`(unmask 真触发)。

## ⭐ 与 NVMe 的关键区别(用户约束核心)

NVMe `init_msi_x`:`mask_all → program_vector → enable → **mask_all**`(最后 mask_all =
polling,test + production 都不真触发,真中断**留 follow-up**)。

VirtIO 批3:`mask_all → program_vector(unmask entry 0)→ enable`,**不 mask_all 回去**。
production(switch_to_apic Step 17 后)真触发。这是"poll 后直接上真中断,不留 follow-up"
(memory [[virtio-real-irq-after-poll-watch-smp]])。

## ⭐ test kernel APIC 限制(诚实)

test kernel(main_test.cpp)无 `switch_to_apic`(只 include local_apic.hpp 给 e1000
poll timer)。如果 test 调 init_msi_x(unmask),MSI 投递到 LAPIC 但软件没 enable → 困
LAPIC ISR(阻塞 e1000 timer,re-NVMe-batch-3 根因2,memory [[f5-m3-nvme-progress]])。

**test_virtio 不调 init_msi_x**(只 transport + round-trip polling);**production main.cpp
Step 21a2 调**(switch_to_apic 后真触发)。

console gate 验:ISR stub 编译 + IDT[0x42] 注册 + round-trip polling 不破(两 leg 888/0)。
production 真触发 GUI 验(用户启动,memory [[gui-verification-user-starts-always]])。

## 验证

- build-verify 全量编绿(KVM)
- run-kernel-test-all 两 leg **888/0** + AP wake PASS
- production main.cpp init_msi_x 编译过(nodiscard 检查 + polling fallback)

## 范围栅栏(诚实)

- **未验真触发**:production MSI 真投递 + ISR 计数(GUI 验,console APIC 限制)。
- **wait_completion 仍 spin**:未换阻塞 wait_queue(ISR 是 wake seam,但 spin polling
  QEMU 仿真够;真硬件省 CPU 留 follow-up——swap spin for prepare_to_wait/schedule_blocked)。
- **单 vector**:virtio-blk 1 queue 1 vector(0x42)。virtio-net 批5 用 3 vector(0x43-0x45)。

接批2 [[2026-07-06-f5-m2-b2-virtio-blk]]。下批:批4 virtio-net 驱动(RX/TX + NetDevice + ping)。
