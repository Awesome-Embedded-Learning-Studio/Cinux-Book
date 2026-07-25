# F5-M2 批2:virtio-blk 驱动 + 3-desc chain round-trip

> 2026-07-06。F5-M2/M7 VirtIO 弧批2。`VirtIOBlock : IBlockDevice`(3-desc chain
> header+data+status via `VirtQueue::submit_chain`)+ read/write round-trip byte-compare。
> 真中断留批3。

## 新增

- `virtio_blk.hpp` / `virtio_blk.cpp` —— `VirtIOBlock`(抄 NvmeBlockDevice 范式)。
  3-desc chain(read:`out=[header] in=[data,status]`;write:`out=[header,data] in=[status]`)。
  单 DmaBuffer 4KiB 布局 `[header 16][data count*512][status 1]`,count 限 7
  (3601<4096,够 ext2 1KiB block count=2)。**Spinlock 从第一天护**(NVMe `749e7db`
  教训:单 DmaBuffer + queue 跨 CPU 不能重叠)。
- `VirtQueue`:加 `submit_chain`(通用 SG,out/in 链 via NEXT)+ 显式 move ctor/assign。
- `main.cpp` Step 21a2:VirtIODevice init + negotiate VERSION_1 + capacity
  (`device_cfg_read64(0)`)+ VirtIOBlock::create + DRIVER_OK + set_virtio_block_device。
  并存(NVMe/AHCI 不动,生产 rootfs 仍 NVMe)。
- `test_virtio`:加 `test_blk_round_trip`(sector 0 write+read byte-compare)。

## ⭐ 踩坑:VirtQueue copy-delete 抑制 move

`VirtQueue` 显式 `copy = delete` 但没声明 move → C++ 规则:显式声明 copy(即使
delete)抑制隐式 move ctor/assign → VirtQueue 无 move ctor → `VirtIOBlock =default`
move 也被 deleted → `expected.hpp` 的 `ErrorOr<VirtIOBlock>`(move 出 create 返回值)
编译错(`use of deleted function`)。

**修**:VirtQueue 加 `VirtQueue(VirtQueue&&) noexcept = default` + 赋值 default。
DmaBuffer move-only 但有 move,default move OK(三 DmaBuffer move + 普通字段浅拷贝)。

**教训**:显式 `copy = delete` 的类,要显式声明 move(default 或自定义),否则 move
被抑制。DmaBuffer 同模式(copy delete + 显式 move default)。VirtIOBlock =default move
现工作(VirtQueue + DmaBuffer + Spinlock 都可 move)。

## 验证

- build-verify 全量编绿(**KVM** 模式,见 [[kvm-available-wsl2]])
- `run-kernel-test-all` 两 leg **888/0**(887 + virtio-blk round-trip 1)+ AP wake PASS
  - `[VirtIO-blk] round-trip OK (512B write+read, capacity=2048 sectors = 1MB test disk)`
  - -smp 2 leg 同(Spinlock SMP 保护 path 验,无并发触发)

## 范围栅栏(诚实)

- **未做真中断**:notify/isr 就位但 MSI-X unmask + ISR 留批3(用户约束:poll 稳后直接上)。
- **Ext2 mount 切换留 perf 批**:生产 rootfs 仍 NVMe,VirtIOBlock 是独立盘机制测。
- **count 限 7**:单 4KiB DmaBuffer。多页传输留续(像 NvmeBlockDevice PRP)。

接批1 [[2026-07-06-f5-m2-b1-virtio-transport]](传输层 + self-assign BAR)。
下批:批3 virtio-blk MSI-X 真中断(0x42)+ ISR flag/wake(不 inline schedule)+ -smp 2 验。
