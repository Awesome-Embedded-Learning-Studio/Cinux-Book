# 2026-07-06 — F5 VirtIO post-finale task 2 SLIRP ping + task 3 perf

> 续 [finale note](2026-07-06-f5-virtio-finale.md)。F5-M2/M7 post-finale 两迭代任务(task 2 SLIRP ping + task 3 perf harness)收官。

## Task 2:virtio-net SLIRP ping 10.0.2.2

**目标**:production `run` 验证 virtio-net 真 SLIRP ping 通(不只 driver init)。

**实现**:
- `net_stack.hpp` `kMaxDevs 2→3`(e1000 + virtio-net + loopback)。
- `virtio_net.hpp/cpp` accessor(`virtio_net_device()`/`set_`)+ `prime_rx()`。
- `net_init.cpp` attach virtio-net(10.0.2.16,e1000 保留 10.0.2.15)+ `dev_for` 优先 virtio-net + ping 用 dev_for。
- `main.cpp` Step 21a3 publish `set_virtio_net_device` + DRIVER_OK 后 `prime_rx`。
- `cmake/qemu.cmake` `run` 加 DEV_VIRTIO_NET(virtio-net-pci + 独立 SLIRP net1)。
- `init.cpp` kernel_init(launch_userspace 前)boot-time ping gate。

### ⭐ 三大坑(逐个诊断定位)

1. **submit_chain NEXT bug**(`virtqueue.cpp`):单 out desc(n_in=0,virtio-net TX)误设 NEXT flag 指 desc[1](未初始化)→ QEMU `virtio: zero sized buffers are not allowed` → frame 丢 → ping `TX=200 RX=0 IRQ=0`。virtio-blk(2 out + 1 in)巧合 work(最后 in desc 不 NEXT)。**修**:`total`-based last desc(`d+1==total`)不 NEXT。

2. **RX buffer 首帧丢失**(`virtio_net.cpp`):`poll_rx` 第一次才 lazy supply RX buffer,但 SLIRP ARP reply 在 poll_rx 跑到前就到 → RX ring 空 → frame 丢 → ping 卡 ARP resolve。**修**:`prime_rx()` 在 create 后(DRIVER_OK 后)pre-fill 一个 RX buffer。

3. **pump_yield 在 kernel_init 不 work**(`init.cpp`):`pump_yield` 假设 user-process syscall 上下文(yield 切 net_poll kthread 排空 RX),但 kernel_init 是 kernel thread,yield 不可靠切 net_poll → reply 不排空 → ping no reply。e1000 A/B 对照证实(e1000 + pump_yield no reply;e1000 + sti/hlt reply)。**修**:kernel_init ping 用 `rx_pump_sti_hlt`(显式 poll,不依赖 net_poll yield);sti/hlt 在 kernel thread safe(无 syscall frame,sys_ping #DF 是 syscall-only)。

### 诊断路径(A/B 对照 + QEMU 报错)

- `TX=200 RX=0 IRQ=0`(virtio)→ device 没收 RX frame。
- e1000 swap(dev_for 临时 e1000)+ pump_yield:no reply → **通用 ping 路径问题**(非 virtio 特有)。
- e1000 + sti/hlt:**reply** → pump_yield 在 kernel_init 调度问题。
- virtio + sti/hlt:`TX=200 RX=0 IRQ=0` → **virtio TX/RX 问题**。
- QEMU 串口 `virtio: zero sized buffers are not allowed` → submit_chain NEXT bug。
- 修 NEXT + prime_rx → virtio + sti/hlt **reply**(TX=2 RX=2 IRQ=2)。

**验证**:production `run`(NVMe boot + virtio-blk + virtio-net + e1000 + xHCI,KVM -smp 2)ping 10.0.2.2 reply(id=0xc1c0 seq=1)。

## Task 3:virtio-blk vs NVMe vs AHCI read perf

**实现**:`init.cpp` boot-time **read-only** micro-bench(per device read 1 block × 256 iters,block `i%64` 循环避 cache,rdtsc 计时)。read-only 安全(NVMe boot disk 不破坏)。

**数据**(build-console production run):

| 设备 | ticks/read | 相对 |
|------|-----------|------|
| NVMe | 448731 | 1.00×(最快)|
| AHCI | 544264 | 1.21× |
| virtio-blk | 595693 | 1.33× |

NVMe vs AHCI **−18%**(与 [M3-b5 note](2026-07-06-f5-m3-b5-perf-nvme-vs-ahci.md) 端到端 gcc −13% 一致;微基准差异略大因无 gcc I/O 重叠)。virtio-blk 最慢(polling + 单 desc chain,无真 IRQ 效率)。

QEMU 仿真三设备都走 block backend,绝对数 QEMU-flavoured;**相对对比**是重点。

## 验证

- `run-kernel-test-all` 两 leg **879 + 889 passed, 0 failed**(submit_chain 改不回归 virtio-blk test)。
- production `run` ping reply + perf 数据,无 panic。

## commit

- `eebbb50` feat(f5-virtio): post-finale task 2 SLIRP ping + task 3 perf harness

## ⭐ 教训

1. **virtio split queue 单 desc chain 也要清 NEXT**:n_in=0 时 out-only chain 的最后(唯一)desc 不能设 NEXT(指 garbage desc → QEMU "zero sized buffers")。multi-desc chain(blk 2+1)巧合 last in desc 清 NEXT 掩盖了 out-only case。新用例(virtio-net TX 1-desc)暴露。
2. **virtio-net RX 需 pre-fill buffer**:device 收 frame 前 ring 要有 buffer,否则丢。lazy supply on first poll 太晚(SLRIP reply 早到)。Linux virtio-net driver 启动填满 RX ring;CinuxOS 单 buffer 模型用 `prime_rx`。
3. **pump_yield 假设 user-process 上下文**:kernel thread ping 要用 sti/hlt pump(显式 poll)。两个 ping 路径(production `sys_ping` user 用 pump_yield + net_poll kthread;boot-time `kernel_init` 用 sti/hlt 自排空)。
4. **A/B 对照诊断**:TX=200 RX=0 IRQ=0 + e1000 swap + sti/hlt swap + QEMU 报错,四步定位(submit_chain NEXT + prime_rx + pump + 通用路径)。每假设仪器验证,不猜。

接 [finale note](2026-07-06-f5-virtio-finale.md) + [IST 根治 note](2026-07-06-f5-msix-ist2-leak.md) + [M3-b5 perf note](2026-07-06-f5-m3-b5-perf-nvme-vs-ahci.md)。
