---
title: Lab 002 · NVMe 与 VirtIO 双驱动验证
---

# Lab 002 · NVMe 与 VirtIO 双驱动验证

> 对应 `document/book/12-storage/002/`。验证档 **A 档**:punchline 是 run-kernel-test 挂上真 `-device nvme`、`-device virtio-blk-pci`、`-device virtio-net-pci`,串口看到三个设备枚举 + 初始化 + 机制测全过。两种 PCI 设备协议(NVMe 队列+doorbell、VirtIO 特性谈判+virtqueue)在同一个内核里并存,各自接回 IBlockDevice / NetDevice。

## 目标

确认五件事:

1. **NVMe 真枚举 + enable**:PCI class 匹配 → BAR0 self-assign → CC.EN↔CSTS.RDY 握手 → Admin SQ/CQ + doorbell;
2. **VirtIO modern transport 走通**:cap 遍历 → 64-bit feature 谈判 → VERSION_1 协商成功(status=0xf);
3. **两种设备 BAR 都 self-assign**(SeaBIOS 靠不住);
4. **多实例 MSI-X** 不撞(xHCI/NVMe/virtio 各占自己的 Table/PBA 槽);
5. **都接回既有抽象**:NVMe + virtio-blk → `IBlockDevice`(Ext2 复用),virtio-net → `NetDevice`(网络栈复用)。

## 步骤

### 1. 两种构建配置都编得过

NVMe 源是无条件编译;VirtIO 源在 `if(CINUX_VIRTIO)` 后(默认 ON)。确认两份都绿:

```bash
cmake -S . -B build -DCINUX_GUI=ON -DCINUX_USB=ON -DCINUX_NET=ON -DCINUX_VIRTIO=ON
cmake --build build -j$(nproc) 2>&1 | grep -iE 'virtio.*\.cpp|nvme.*\.cpp|Built target big_kernel_test' | head
# 非 GUI 也验一遍(init.cpp 的 NVMe boot-disk 逻辑在 boot 路径上):
cmake -S . -B build-console -DCINUX_GUI=OFF -DCINUX_VIRTIO=ON
cmake --build build-console -j$(nproc) 2>&1 | grep -iE 'error|Built target big_kernel_test' | tail
```

应看到 `virtio.cpp`/`virtqueue.cpp`/`virtio_blk.cpp`/`virtio_net.cpp`/`nvme.cpp` 都编了,无 error。

### 2.(A 档 punchline)run-kernel-test 挂真设备看探测

`QEMU_TEST_EXTRA_FLAGS` 已经挂了 nvme + virtio-blk + virtio-net 三种设备(配 1 MB 测试盘),跑:

```bash
cmake --build build --target run-kernel-test 2>&1 | grep -aE 'PCI\].*(NVMe|VirtIO)|NVMe\]|VirtIO\]|Tests:|ALL TESTS' | head -30
```

串口上应按顺序看到:

```
[PCI] NVMe found: 00:04.0 BAR0=0x0
[NVMe] enabled (admin queue=64 RDY=1 doorbell stride=4)      ← doorbell stride=4(DSTRD 解码对)
[NVMe] MSI-X enabled (entry 0 -> vector 0x41, 65 entries)    ← 多实例 MSI-X,NVMe 用 vector 0x41
[PCI] VirtIO-blk found: 00:05.0 BAR0=0xc000
[VirtIO] transport ready: common BAR4+0x0 notify BAR4+0x3000 (mult=4) isr=Y device=Y num_queues=1 ...
[VirtIO] transport OK: negotiated=0x100000000 status=0xf     ← VERSION_1(bit32)协商成功,status 全亮
[PCI] VirtIO-net found: 00:06.0 BAR0=0xc100
[VirtIO] transport ready: ... num_queues=3                    ← RX/TX/ctrl 三队列
...
Tests: 1065 passed, 0 failed                                  ← 075 的 1061 + 4 个新驱动测试
```

`doorbell stride=4` 是这章头号坑的正面证据——DSTRD 解码对了(解错会变 128 KiB,门铃敲出 BAR 外)。`nic virtio-net0 has no peer` 是预期警告(测试只挂设备无 SLIRP netdev)。

### 3. grep 锚点:两条主线 + 共性基建

```bash
# (a) PCI 识别:NVMe 用 class,VirtIO 用 vendor/device
grep -n 'is_nvme_device\|is_virtio_block_device\|is_virtio_net_device' kernel/drivers/pci/pci.hpp

# (b) BAR self-assign(两驱动共用的坑)
grep -n 'self_assign_bar\|0xfeb40000\|0xfeb60000\|write_bar.*0xFFFFFFFF' kernel/drivers/nvme/nvme.cpp kernel/drivers/virtio/virtio.cpp

# (c) 多实例 MSI-X(061 的 init 覆盖参数被这两驱动消费)
grep -n 'table_virt\|pba_virt\|kNvmeMsixTableVirt\|0x74000' kernel/drivers/pci/msix_controller.cpp kernel/drivers/nvme/nvme.cpp

# (d) NVMe 队列对 + doorbell
grep -n 'doorbell\|DSTRD\|cap_lo >> 28\|cq_phase\|Submission\|admin_sq' kernel/drivers/nvme/nvme.cpp

# (e) VirtIO status 机 + split virtqueue
grep -n 'FEATURES_OK\|DRIVER_OK\|negotiat\|avail\|used\|kick\|notify_off' kernel/drivers/virtio/virtio.cpp kernel/drivers/virtio/virtqueue.cpp

# (f) 都接回 IBlockDevice / NetDevice
grep -n 'IBlockDevice\|NetDevice' kernel/drivers/nvme/nvme_block_device.hpp kernel/drivers/virtio/virtio_blk.hpp kernel/drivers/virtio/virtio_net.hpp
```

### 4. 机制测试增量

```bash
cmake --build build --target run-kernel-test 2>&1 | grep -aE 'test_nvme|test_virtio' | head
```

应看到 `test_nvme`、`test_virtio`(含 `test_net_bringup`)一批 PASS——这些是 002 新加的驱动机制测,挂真设备跑(不是 stub)。

## 想一想

1. **NVMe 的 doorbell stride 解码**:如果把 CAP 的 `(cap_lo >> 28) & 0xF` 错写成 `(cap_lo >> 24) & 0xF`,会解出 DSTRD=15。算一下 stride 会变成多少?第二个队列的门铃偏移会落在哪里?为什么这会让驱动崩?
2. **VirtIO 的 BAR 为什么得 self-assign?** 提示:SeaBIOS 给 modern BAR 配了正确的 type bits,但 upper 32 位是垃圾;`read_bars` 把它当 64-bit BAR 合并出什么物理地址?映射过去读到什么?
3. **NVMe 和 virtio-blk 都实现 IBlockDevice,Ext2 却一行不改**——这是哪一章立抽象换来的好处?如果当初 Ext2 直接调 AHCI 寄存器,加 NVMe 会怎样?
4. **NVMe 的 CQ 完成项有 phase 位、virtqueue 的 used idx 是 free-running**——两种"怎么知道有新完成"的机制,各自的回绕处理有什么不同?
