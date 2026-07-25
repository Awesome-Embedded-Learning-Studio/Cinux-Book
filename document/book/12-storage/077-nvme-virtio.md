---
title: 077 · NVMe 与 VirtIO:两种 PCI 设备抽象并存
---

# 077 · NVMe 与 VirtIO:两种 PCI 设备抽象并存

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

## 共同的起点:PCI 枚举 + BAR self-assign + 多实例 MSI-X

三个驱动一开始都走同一条 PCI 路,咱们先把共性拎出来。

**枚举靠 class 或 vendor/device 匹配**。[pci.hpp](kernel/drivers/pci/pci.hpp) 里给每种设备一个识别谓词:

```cpp
constexpr bool is_nvme_device(uint8_t cls, uint8_t sub) {
    return cls == 0x01 && sub == 0x08;            // 大类 0x01=存储,子类 0x08=NVMe
}
constexpr bool is_virtio_block_device(uint16_t vendor, uint16_t device) {
    return vendor == 0x1AF4 && (device == 0x1001 || device == 0x1042);  // VirtIO 厂商 + blk 设备号
}
```

NVMe 用 **class code**(同类设备共用一个类号,不管哪家厂);VirtIO 用 **vendor/device**(VirtIO 厂商号 0x1AF4 是固定的,设备号区分 blk/net)。`PCI::find_nvme` / `find_virtio_block` / `find_virtio_net` 各自扫总线,命中就读 BAR、回填 `PCIDevice`。

**BAR self-assign 是这一章两个驱动共同的坑,值得先讲**。PCI 设备的 BAR(Base Address Register)告诉驱动它的寄存器窗口想开在哪段物理地址。正常流程是 BIOS(咱们这边是 SeaBIOS)在启动时给每个 BAR 分配好地址,驱动直接读。可现实是:

- NVMe 的 BAR0,SeaBIOS 给了全 0(没分配)。
- VirtIO 的 modern BAR 是 64-bit prefetchable,SeaBIOS 给了正确的 type bits,却配了一坨垃圾高位(`BAR4=0x4000000c`、`BAR5=0xc0`),`read_bars` 把它当 64-bit BAR 合并出 `0xc040000000`(~823 GB),映射过去落在被 poison 填掉的 RAM 区,读寄存器返回 `0xcafebabecafebabe`。

两个驱动的解法一样——**别信 BIOS,自己来**:

```cpp
// 1. 探 size:往 BAR 写全 1,读回的补码编码就是 size
write_bar(bar, 0xFFFFFFFF);
uint32_t raw = read_bar(bar);
uint64_t size = ~(raw & size_mask) + 1;       // NVMe BAR0 = 16384(16 KiB)

// 2. 写一个自己挑的固定 32-bit 槽(避开已知设备的 BAR)
write_bar(bar, 0xfeb40000);                    // NVMe;VirtIO 用 0xfeb60000
write_upper_bar(bar, 0);                       // 64-bit BAR 的 upper 写 0(< 4 GB)

// 3. 更新 dev.bar[],后面 map_bar 用这个
```

> 教训记一笔:**PCI BAR 不能假定 BIOS 分配好了**。读到 poison(全 `0xcafebabe` 之类)或全 0,第一反应该是"BAR 没分对",不是"设备坏了"。两个驱动都吃了这个亏,所以把 self-assign 抽成共用动作。

**多实例 MSI-X**。三个设备各要自己的 MSI-X 中断,而 MSI-X Table/PBA 得映射到不撞的虚拟地址槽。061 章给 [msix_controller.cpp](kernel/drivers/pci/msix_controller.cpp) 的 `init` 加了覆盖参数:

```cpp
const uint64_t table_v = (table_virt != 0) ? table_virt : kMsixTableVirt;
const uint64_t pba_v   = (pba_virt != 0)   ? pba_virt   : kMsixPbaVirt;
```

xHCI 用默认槽(`+0x40000`/`+0x41000`),NVMe 传 `+0x74000`/`+0x75000`,virtio-blk/net 各再往后挪——同一套 MSI-X 基建,多设备共存。这部分是 061 打好的地基,这一章只是消费它。

## 主线一:NVMe——自己造队列 + 敲 doorbell

NVMe 把"提交命令、拿结果"这件事做成了**队列对**:你维护一个 Submission Queue(SQ)塞命令,设备维护一个 Completion Queue(CQ)写完成。你敲 SQ 的 doorbell 告诉设备"有新命令了",设备干完敲 CQ(写一条完成项 + 你轮询)。

### enable():CC.EN ↔ CSTS.RDY 握手

NVMe 控制器启用是个状态机([nvme.cpp](kernel/drivers/nvme/nvme.cpp) 的 `enable()`):先 disable(写 `CC.EN=0` → 轮询等 `CSTS.RDY=0`),配好 Admin SQ/CQ 的物理地址和大小(写 AQA/ASQ/ACQ 寄存器),再 `CC.EN=1` → 轮询等 `CSTS.RDY=1`。握手成了才算是"控制器就绪":

```
[NVMe] enabled (admin queue=64 RDY=1 doorbell stride=4)
```

Admin SQ/CQ 用 `DmaPool` 分配(64 项 × 64 B 的 SQ + 64 项 × 16 B 的 CQ,4 KiB 页对齐),`NvmeController` 持 `DmaBuffer` 成员 → move-only。

### doorbell + Identify:塞命令、敲门铃、轮询 phase

发一条 Identify Controller 命令的流程,把 NVMe 的交互模型整个走一遍:

1. 构造命令(op 0x06 / CNS=0x01 / PRP1 指向一个 4 KiB DMA 缓冲)塞进 `Admin SQ[tail]`,`tail++`(回绕)。
2. **写 SQ tail doorbell**——告诉设备"tail 到这了,有新命令"。每个队列占**两个** stride 槽(SQ tail 在偶数槽、CQ head 紧跟在奇数槽),所以 SQ tail doorbell 的偏移 = `0x1000 + 2 × queue_id × stride`,CQ head doorbell 紧跟在 `0x1000 + (2 × queue_id + 1) × stride`,stride = `4 << DSTRD`(见 [nvme.hpp](kernel/drivers/nvme/nvme.hpp) 的注释:`0x1000 + queue_id * (2 * stride)`)。
3. 轮询 `CQ[head]` 的 phase 位。NVMe 规定 CQ 完成项的 status 最低位是 phase,初值 1;每回绕一圈(head 归零)`cq_phase_ ^= 1`。`CQE.status bit0 == cq_phase_` 就说明这是一条新完成。
4. `status >> 1 == 0` → 成功,4 KiB 缓冲里就是 controller data(VID/SN/MN)。

### ⭐ doorbell stride 解码坑

这一步有个值钱的坑。CAP 寄存器的 doorbell stride 字段 DSTRD 在 CAP 高 32 位的 `bits[31:28]`。早期代码写的是 `(cap_lo >> 24) & 0xF`——把 `bits[27:24]`(其实是 TO 字段的高位)当成了 DSTRD,解出 15。

错成 15 会怎样?stride = `4 << 15` = 128 KiB。SQ tail doorbell 偏移 = `0x1000 + 2 × queue_id × 128KiB`,第二个队列(qid=1)的门铃就敲到 BAR0 那 16 KiB 窗口外面去了——访问越界。正解是 `(cap_lo >> 28) & 0xF` = 0,stride = 4(标准值):

```
[NVMe] enabled (admin queue=64 RDY=1 doorbell stride=4)
```

> 教训:**NVMe CAP 字段的位宽对 spec 版本敏感**。DSTRD 在 NVMe 1.4 是 `bits[31:28]` 的高 4 位,而它前面 `bits[27:23]` 是 TO(超时)。把 TO 的高位误当 DSTRD,stride 就飞了。读 spec 寄存器,字段边界要对着版本核,别凭印象移位。

### IO 队列 + PRP + IBlockDevice

Admin 队列只能发管理命令(Identify、Create IO Queue)。真读写要走 **IO Submission/Completion Queue**:`create_io_queues()` 建一对 IO 队列,数据读写命令进 IO SQ。数据地址用 **PRP**(Physical Region Page)——PRP1 指向一个物理页(这一章只做单页 R/W,PRP2 链留 follow-up)。

最后用 [nvme_block_device.cpp](kernel/drivers/nvme/nvme_block_device.cpp) 把"发一条 NVMe 读写命令、轮询 IO CQ 拿结果"包成 `IBlockDevice`——Ext2 见到的就是一个能 read/write 块的设备,跟 AHCI 那个一模一样。`init.cpp` 启动时 prefer NVMe(性能路径),挂不上就退 AHCI:

```
[INIT] rootfs on NVMe (perf path)    // 或 [INIT] rootfs on AHCI
```

## 主线二:VirtIO——谈判特性 + 共享环

VirtIO 是虚拟化环境(KVM/QEMU)里的标准设备协议。它的思路跟 NVMe 完全不同:**不定义命令格式,而是谈判双方都支持哪些特性,然后共享一组叫 virtqueue 的描述符环**。

### modern transport:cap 遍历 + 特性谈判

VirtIO PCI modern 设备把寄存器藏在 PCI capability list 里([virtio.cpp](kernel/drivers/virtio/virtio.cpp))。遍历 cap list 找四种 cfg:`common`(`cfg_type=1`)、`notify`(2)、`isr`(3)、`device_cfg`(4),各映射到 modern BAR 窗口的一个 4 KiB 区(common+0x0 / isr+0x1000 / device+0x2000 / notify+0x3000)。

然后走 **status 机**跟设备谈判:

```
ACK | DRIVER  →  读 device feature(两个 32-bit word)  →  写咱们要的 feature AND
            →  FEATURES_OK  →  setup_queue  →  DRIVER_OK
```

feature 是 64-bit(两个 32-bit word,先读 device 的,跟咱们支持的 AND 一把写回去)。谈判成功的标志:`negotiated=0x100000000`(VERSION_1,bit 32)、`status=0xf`(ACK|DRIVER|FEATURES_OK|DRIVER_OK 全亮):

```
[VirtIO] transport ready: common BAR4+0x0 notify BAR4+0x3000 (mult=4) isr=Y device=Y num_queues=1 features=0x10130006e54
[VirtIO] transport OK: negotiated=0x100000000 status=0xf
```

### split virtqueue:desc / avail / used 三个环

VirtQueue 是 **split virtqueue**——三块 DMA 内存([virtqueue.cpp](kernel/drivers/virtio/virtqueue.cpp)):

- **desc[]**:描述符数组,每个描述符说"这段物理地址、这么长、可写/可读"。
- **avail[]**:驱动写给设备的——"我放了这些 desc 给你,head 索引在 avail[idx]"。
- **used[]**:设备写给驱动的——"我消费了这些,idx 追到这了"。

发一个请求:填 desc(virtio-blk 一个请求是 3-desc 链:请求头 + 数据 + 状态),把 head 写进 avail,更新 `avail->idx`,然后 **kick**(写 notify 寄存器:`notify_base + notify_off × mult`)通知设备。等 `used->idx` 追上目标,就是完成了。

> virtqueue 的 `used->idx` 是 free-running(一直涨,不回绕取模),比 NVMe CQ 的 phase 位简单——你只比 idx 追没追上,不用管回绕翻转。

### blk + net:同一个 transport,两种设备

传输层(VirtIODevice + VirtQueue)是共用的,上面挂两种设备:

- **virtio-blk**([virtio_blk.cpp](kernel/drivers/virtio/virtio_blk.cpp)):每个请求 3-desc 链(请求头 / 数据缓冲 / 状态字节),read/write 方向在请求头里。同样包成 `IBlockDevice` 挂 Ext2。
- **virtio-net**([virtio_net.cpp](kernel/drivers/virtio/virtio_net.cpp)):RX/TX 各一个 virtqueue(设备上报 `num_queues=3`,含一个 ctrl 队列,但驱动这一章只建 RX+TX 两个)。包成 `NetDevice` 接网络栈,`net_init.cpp` 里它跟 e1000 并存,`dev_for()` 优先用它(`ping 10.0.2.2` 走 virtio RX/TX 验证)。

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
