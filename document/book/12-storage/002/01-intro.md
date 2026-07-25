---
title: 01 · 导引:点亮什么、为什么
---

# 导引:点亮什么、为什么

> 039 那会儿咱们给 AHCI 写过一个块设备驱动——硬盘就是"往端口写命令、等中断、搬数据"。可 PCI 设备远不止这一种脾气。这一章一口气加两个**风格完全不同**的 PCI 设备驱动,让它们在同一个内核里并存:**NVMe**(PCIe SSD,自己造一套提交/完成队列、你敲 doorbell)和 **VirtIO**(虚拟化标配,跟设备谈判特性、共享一个环)。
>
> punchline 是个对比:**面对一块 PCI 设备,硬件设计师给了两条截然不同的路**。NVMe 走"我自己定义队列 + doorbell 寄存器"——你把命令塞进 Submission Queue,敲一下门铃(SQ tail doorbell),设备干完了往 Completion Queue 里写一条,你轮询那个环。VirtIO 走"咱们先谈判支持哪些特性,然后共享一组 virtqueue 描述符环"——你填描述符链、更新 avail 索引、kick 一下,设备消费完更新 used 索引。两条路最后都接到咱们已有的抽象上(NVMe 和 virtio-blk 都变成 `IBlockDevice`,挂 Ext2;virtio-net 变成 `NetDevice`,接网络栈)。
>
> A 档:punchline 是 run-kernel-test 里挂上真 `-device nvme`、`-device virtio-blk-pci`、`-device virtio-net-pci`,串口能看到三个设备都被枚举、初始化、跑通机制测——`[NVMe] enabled ... doorbell stride=4`、`[VirtIO] transport OK: negotiated=0x100000000 status=0xf`。

## 这章咱们要点亮什么

1. **PCI 设备没有统一脾气**:AHCI 是"端口 + 中断",NVMe 是"队列 + doorbell",VirtIO 是"特性谈判 + 共享环"。同一个内核得能用三套完全不同的设备协议。
2. **NVMe 的核心是 Submission/Completion Queue 对**:命令进 SQ、敲 doorbell、轮询 CQ 的 phase 位拿完成。门铃步长(stride)由 CAP 寄存器的 DSTRD 字段定——这个字段解码错,门铃就敲到 BAR 外面去了。
3. **VirtIO 的核心是特性谈判 + virtqueue**:先走 status 机(ACK→DRIVER→谈判 feature→FEATURES_OK→DRIVER_OK),再配 split virtqueue(desc/avail/used 三个环),kick 通知设备。
4. **SeaBIOS 的 BAR 分配靠不住**:NVMe 的 BAR0、VirtIO 的 modern BAR,SeaBIOS 都可能不正经分配(给全 0,或给正确的 type bits 配垃圾高位)。两个驱动都得**自己探 size + 自己写一个固定槽** self-assign,否则映射到非法物理地址、读到 poison 值。
5. **复用 061 的多实例 MSI-X**:NVMe、virtio-blk、virtio-net 各要自己的 MSI-X Table/PBA 映射槽,不能撞 xHCI 的。061 给 `MsixController::init` 加了 `table_virt`/`pba_virt` 覆盖参数,这一章正好用上。
