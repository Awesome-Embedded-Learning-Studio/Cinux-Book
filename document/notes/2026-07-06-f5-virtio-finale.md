# F5-M2/M7 VirtIO 弧收官(blk + net + 真中断路径)

> 2026-07-06。F5-M2/M7 VirtIO 块设备+网卡弧全收官(6 commit,`feat/f5-virtio-blk-net`)。
> VirtIO modern transport + virtio-blk(完整 + 真中断)+ virtio-net(driver + 真中断路径)。

## 6 批 commit

- 批0 `74eb6e3`:立项 + 接线(PCI find_virtio + QEMU virtio-blk-pci + CMake option)
- 批1 `f780a36`:传输层 + virtqueue polling(⭐ self-assign BAR 踩坑:SeaBIOS 不分配 virtio-pci modern BAR,bogus 64-bit 标记 + 垃圾 upper → 读 poison 0xcafebabecafebabe)
- 批2 `cb18bc8`:virtio-blk 3-desc chain + IBlockDevice round-trip(⭐ VirtQueue copy-delete 抑制 move 踩坑)
- 批3 `3028d05`:virtio-blk MSI-X 真中断路径(unmask + ISR + IDT[0x42],不像 NVMe mask_all polling)
- 批4 `fbf2cc2`:virtio-net driver(RX/TX + NetDevice + bringup)
- 批5:virtio-net 真中断(单 vector 0x43)+ production 注册 + ROADMAP F5-M2/M7 ✅

## 弧核心交付

- **VirtIO modern transport**(`virtio.hpp/cpp`):PCI cap 遍历(common/notify/isr/device_cfg)+
  64-bit feature 协商 + status 机 + queue 配置 + **self-assign BAR**(批1 踩坑)。
- **virtqueue**(`virtqueue.hpp/cpp`):split queue(desc/avail/used)+ submit_one/submit_chain(SG)
  + wait/has/consume(blk spin / net poll)+ move ctor。
- **virtio-blk**(`virtio_blk.hpp/cpp`):`IBlockDevice` 适配,3-desc chain(header+data+status)
  + 单 DmaBuffer + **Spinlock 从第一天护**(NVMe `749e7db` 教训)。MSI-X 真中断(0x42,unmask)。
- **virtio-net**(`virtio_net.hpp/cpp`):`NetDevice` 适配,RX/TX virtqueue + fixed virtio_net_hdr +
  poll_rx(supply/consume)+ send_l3(**手动 wire 序**,EthHdr 是 host-order view 非 overlay)。
  MSI-X 真中断(单 vector 0x43,RX/TX 共享)。
- **production 真中断路径**:main Step 21a2(blk)+ 21a3(net)调 init_msi_x(unmask)。test kernel
  不调(APIC 限制,无 switch_to_apic → 困 ISR,见批3 note)。

## ⭐ 用户约束落实(memory [[virtio-real-irq-after-poll-watch-smp]])

- ✅ poll 稳后直接上真中断(批3 blk + 批5 net,production unmask,不留 follow-up)
- ✅ ISR 不 inline schedule(handler 只计数,避 sti-in-syscall #DF)
- ✅ 共享状态 Spinlock 从第一天护(blk/net 都加)
- ✅ -smp 2 验(run-kernel-test-all 两 leg 含 -smp 2,全绿)

## 验证

- run-kernel-test-all 两 leg **889/0**(886 基线 + transport/blk-roundtrip/net-bringup 3)+ AP wake
- **KVM 模式**(build-verify -DCINUX_USE_KVM=ON,见 [[kvm-available-wsl2]])
- production 真中断编译过(nodiscard 检查 + polling fallback)

## ⭐ follow-up(诚实,非弧阻塞)

- **NetStack attach + SLIRP ping**:virtio-net driver 就位但 production net_init 仍 e1000。
  attach virtio-net(替换/补充 e1000)+ SLIRP ping 10.0.2.2 是集成 gate(GUI 验)。
- **perf 对比**:virtio-blk vs NVMe vs AHCI I/O(production rootfs + gcc 编译,GUI perf gate)。
- **多向量**:net 当前单 vector 0x43(RX/TX 共享);3 向量(0x43-0x45)+ per-queue queue_msix_vector。
- **mergeable buffer / CSUM / GSO**:fixed header(无 offload)。
- **wait_queue 阻塞**:wait_completion 仍 spin;阻塞(schedule_blocked + ISR wake)省 CPU 真硬件。
- **packed virtqueue / scatter-gather 多链**。

ROADMAP F5:**M2 ✅** + **M7 ✅**(driver + 真中断路径;attach/perf follow-up)。8 驱动。

接 [[virtio-real-irq-after-poll-watch-smp]](用户约束,本弧落实)+ [[f5-m3-nvme-progress]](NVMe 弧,self-assign BAR + NvmeBlockDevice Spinlock 同根因)。F5-M2/M7 ✅。
