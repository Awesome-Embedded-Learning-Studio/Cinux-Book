---
title: 03 · 落点与诚实的边界
---

# 落点与诚实的边界

## 落点:都接回既有抽象

这章加了一堆新代码(NVMe 控制器 + 块设备、VirtIO 传输层 + virtqueue + blk + net),但**对外接口零新增**:

- NVMe 和 virtio-blk 都实现 `IBlockDevice`——Ext2 不关心盘是 AHCI、NVMe 还是 virtio,`read_block`/`write_block` 一样调。
- virtio-net 实现 `NetDevice`——网络栈的 `dev_for()` 在它和 e1000 之间挑,上层 ICMP/UDP/TCP 无感知。

这就是抽象的价值:**设备协议再怎么花样翻新,只要能翻译成 `IBlockDevice` / `NetDevice`,上面的 Ext2 和网络栈一行不改**。038 章立的 `IBlockDevice`、058 章立的 `NetDevice`,到这里各收一个新实现。

## 诚实的边界

- **NVMe 只做单页 R/W(PRP1)**:PRP2 链(跨页大块传输)留 follow-up。IO 队列是**轮询**模式(init_msi_x mask 掉每个 entry,IDT[0x41] stub 不真触发),真异步 IRQ 留后续。
- **VirtIO 是 split virtqueue + polling**:`submit_one` 是单描述符(virtio-blk 的 3-desc 链在 blk 层组),`wait_completion` 轮询 used idx。packed virtqueue、中断回调(DRIVER_OK 后真用 MSI-X vector 0x42/0x43)这一章只到"stub 注册 + 计数",真中断驱动留后续。
- **virtio-net 流量没在 run-kernel-test 验**:测试只挂设备(无 SLIRP netdev,所以有 `nic virtio-net0 has no peer` 警告),验的是 bring-up + MAC + 队列配置。真 `ping 10.0.2.2` 走 virtio 是生产路径(要给 virtio-net 单独挂 SLIRP netdev),留 follow-up。
- **production rootfs 仍在 AHCI/NVMe**:virtio-blk 在 `run` 里是独立的第三块盘(不是启动盘),验的是"设备枚举 + transport + IBlockDevice 创建 + DRIVER_OK"这条链真能跑。
