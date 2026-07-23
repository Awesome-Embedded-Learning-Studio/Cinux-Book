# F5-M2 批4:virtio-net 驱动

> 2026-07-06。F5-M2/M7 VirtIO 弧批4。`VirtIONetDevice : NetDevice`(RX/TX virtqueue +
> poll_rx + send_l3 wire 序)。机制测 bring-up(create + MAC + RX/TX queue config)。
> SLIRP ping 留批5/production(net_init 改 + netdev)。

## 新增

- `virtio_net.hpp/cpp`:`VirtIONetDevice : cinux::net::NetDevice`。RX queue 0 + TX queue 1
  (VirtQueue 复用)。`poll_rx`(supply writable buffer → `has_completion` → consume + 借
  frame bytes 给 Packet.data;copy 语义 sink=nullptr)+ `send_l3`([virtio_net_hdr 12 |
  EthHdr wire 14 | l3],**手动写 wire 序**因 EthHdr 是 host-order view 非 wire overlay,
  见 net_types.hpp)。fixed virtio_net_hdr 12B(无 mergeable/CSUM/GSO)。Spinlock 护 RX/TX
  (同 NVMe 749e7db)。
- `VirtQueue` 加 `has_completion()` + `consume_completion()`(RX 非阻塞 poll,net_poll
  kthread 范式;blk 的 wait_completion spin 不适用 RX)。
- CINUX_VIRTIO gate 加 virtio_net.cpp。QEMU 加 virtio-net-pci(无 netdev,机制测)。
- `test_net_bringup`:create + MAC 非零 + RX/TX queue init。

## 验证

- build-verify 全量编绿(KVM)
- run-kernel-test-all 两 leg **889/0**(888 + virtio-net bringup)
  - `[VirtIO-net] bringup OK MAC=52:54:00:12:34:56`(单核)/ `...57`(-smp 2)
  - num_queues=3(RX/TX/ctrl),features=0x10130bf8024(net feature 含 MAC)

## 范围栅栏(诚实)

- **未做 SLIRP ping**:test 只验 create + MAC + queue init(机制测)。SLIRP ping 要
  net_init 改(attach virtio-net + netdev 配)留批5/production。QEMU virtio-net-pci 没挂
  netdev(test 不收发)。
- **未做真中断**:RX/TX polling(net_poll kthread 范式)。真中断(3 向量 0x43-0x45)留批5。
- **fixed header**:无 mergeable buffer(单 buffer RX,一帧一 poll)。CSUM/GSO 不协商
  (软件校验)。

接批3 [[2026-07-06-f5-m2-b3-virtio-blk-irq]]。下批:批5 virtio-net 真中断(3 向量)+
SMP + perf + ROADMAP ✅ 收官。
