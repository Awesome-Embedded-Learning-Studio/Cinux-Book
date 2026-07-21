---
title: 038 · IBlockDevice 块设备抽象
---

# 038 · IBlockDevice:让 ext2 不再死绑在 AHCI 上

> 这是 F1 基建弧的最后一块。前面 036 立了 Cinux-Base + ErrorOr,037 收了 RingBuffer/dmesg/DMA 池;这一章立**块设备抽象** `IBlockDevice`,把 ext2 从 AHCI 的硬编码里摘出来。动机很直白:ext2 的构造函数现在写死了 `Ext2(AHCI&, port)`,哪天笔者想给它接块 NVMe 盘、或者一个 VirtIO-blk,就得回头改 ext2 本体——这不对。文件系统不该知道盘是什么控制器。
>
> B 档:这一弧没有新的用户可见现象,验证靠构建 + 测试 + 看接口形状。但它给后面的 F5(NVMe/VirtIO 驱动)和 F2(Page Cache)留好了统一接入口。

## 这章咱们要点亮什么

一个接口,两个实现,外加一次解耦:

1. **`IBlockDevice`**——最小同步块设备接口(`read_blocks/write_blocks/flush/block_count/block_size`),纯内核内部,走 `ErrorOr<void>`。
2. **`RAMBlockDevice`**——内存实现,当测试桩和 ramdisk 后备。
3. **`AHCIBlockDevice`**——AHCI 的薄适配器,持一个 `DmaBuffer`,把老 `AHCI::read/write` 包成 `IBlockDevice`。
4. **ext2 解耦**——构造函数从 `Ext2(AHCI&, port)` 换成 `Ext2(IBlockDevice*)`,自带的散装 DMA 也淘汰,换成一块固定数组 `block_buf_[4096]`。

## 为什么需要它

037 之前 ext2 长这样:构造函数直接吃一个 `AHCI&` 和端口号,自己还揣着一摊 ad-hoc DMA(`g_pmm.alloc_page` + `g_vmm.map(EXT2_DMA_VIRT_BASE)` + `dma_buf_phys_/virt_/dma_ready_/ensure_dma_buffer` 一整套)。这是 036/037 还没收口时的遗留——和 F1-M3 收掉的散装 DMA 是同一种病,只不过这一份长在 ext2 里。

问题不在"能跑",在**耦合**:文件系统层知道了太多控制器的细节。后果是——

- 想接第二块盘(NVMe、VirtIO-blk)进 ext2,得改 ext2 构造函数;
- ext2 的 DMA 逻辑和 AHCI 的 DMA 逻辑各写一份,口径不一;
- 后面 F2 要做 Page Cache,需要一个统一的"读块"入口,找不到。

所以这一弧抽一个**最小同步**块设备接口出来,让 ext2 只认接口、不认控制器。注意是"同步"——请求队列、异步 IO 这些先不碰(留后续),现在只要"给块号、读/写、返 ErrorOr"。

## IBlockDevice:接口长什么样

`kernel/drivers/block_device.hpp:44`:

```cpp
class IBlockDevice {
public:
    // 传输 [block, block+count) 个设备块到/从 buf。buf 是普通虚拟地址,
    // 大小 count * block_size() 字节。失败返 ErrorOr<void>(Error::IOError)。
    virtual cinux::lib::ErrorOr<void> read_blocks(uint64_t block, uint64_t count, void* buf) = 0;
    virtual cinux::lib::ErrorOr<void> write_blocks(uint64_t block, uint64_t count, const void* buf) = 0;
    // 几何查询(块数 / 块大小);flush 当前默认空,留真命令给以后。
    ...
};
```

一个**故意**的设计选择:接口走 `ErrorOr<void>`,不走 `bool`。理由和 036 一脉相承——这是个会失败的 IO 操作,失败原因(IO 错)值得有名字、有类型,而不是一个 `false` 让调用方猜。它是纯内核内部接口(不跨 syscall trap),所以可以直接用 `ErrorOr`,不用像 syscall 边界那样翻成 `-errno`。

> 这里有条小妥协:ext2 **内部**的 `read_block` 仍保 `bool` 返回(渐进迁移,同 036 的 M0),只有调 `dev_->read_blocks` 那一层用 `ErrorOr`。一次性把 ext2 内部全翻 ErrorOr 是另一个里程碑的活,这里只翻接口。

## 两个实现

**`RAMBlockDevice`**(`kernel/drivers/ram_block_device.hpp:36`)——`IBlockDevice` 的内存实现,底层是 `Heap::alloc/free` 配对,move-only。它干两件用:一是当**测试桩**(单测里给 ext2 喂一块假盘,不用真 AHCI);二是给 ramdisk 当后备。

**`AHCIBlockDevice`**(`kernel/drivers/ahci/ahci_block_device.hpp:41`)——AHCI 的薄适配器。它**不碰 `ahci.cpp` 本体**(那个文件里的 ad-hoc DMA 迁 DmaPool 是 F5-M1 的活),只在外面包一层:持一个 `DmaBuffer`(从 037 的 `g_dma_pool` 来),`read_blocks` 走 DMA phys → virt memcpy 到 buf,`write_blocks` 反向。

```cpp
// kernel/drivers/ahci/ahci_block_device.hpp:41
class AHCIBlockDevice : public cinux::drivers::IBlockDevice { ... };
```

注意它有个静态工厂 `AHCIBlockDevice::create(AHCI&, port)` 返 `ErrorOr<AHCIBlockDevice>`——构造可能失败(端口没盘),用 ErrorOr 表达。

## ext2 解耦

ext2 这边的改动(`kernel/fs/ext2.hpp`):

```cpp
// kernel/fs/ext2.hpp:52 —— 构造函数只认接口
explicit Ext2(cinux::drivers::IBlockDevice* dev);

// kernel/fs/ext2.hpp:366 —— 自带 DMA 淘汰,换成固定 scratch 数组
cinux::drivers::IBlockDevice* dev_;
uint8_t block_buf_[4096];
```

那一整套 `dma_buf_phys_/virt_/dma_ready_/ensure_dma_buffer` 全删了。`block_buf_` 是个 4096 字节的固定数组(容纳任何 ext2 block,免 heap 分配和析构),替代原来 DMA 映射出来的 `dma_buf_virt_`。DMA 的事下放给 `AHCIBlockDevice`——ext2 不再关心"块怎么从盘上搬过来",只说"把第 N 块搬过来"。

接线处在 `kernel/proc/init.cpp`:

```cpp
static auto blk_dev =
    cinux::drivers::ahci::AHCIBlockDevice::create(cinux::drivers::ahci::AHCI::instance(), 1);
static cinux::fs::Ext2 ext2(blk_dev.ok() ? &blk_dev.value() : nullptr);
auto                   mount_result = ext2.mount();
```

> 这一段在回迁时**冲突过**:`init.cpp` 这处 Book 和 CinuxOS 的 context 没对齐(Book 还是老的直挂 AHCI 构造),patch 打不上。解法是取 CinuxOS 侧(就是上面这段)——这正是 patch-replay 里"偶尔 context 漂移,手收一下"的正常情况,不是机制出问题。

## 踩坑:QEMU AHCI 容量不等于 ext2.img 文件大小

这是这一弧最坑爹的一个发现(CinuxOS PLAN 里记成 GOTCHA #8)。批 2 做 AHCI round-trip 写测试时,挑了 sector 7000 写——`ext2.img` 文件是 8192 个 sector,7000 明明在范围内,结果 `[AHCI] command timeout`,盘根本没响应。

查下来:**QEMU AHCI IDENTIFY 报告的盘容量几何,小于 `ext2.img` 文件的实际大小**。也就是说文件层面"看起来有 8192 sector",但 QEMU 给 AHCI 控制器模拟的几何更小,写到越界 sector 它就不理你。

修复不是去改 QEMU,而是换测试策略:真机写测试用**已知可写的低 sector**(比如 sector 2000),而且先读原值、写完写回 restore,不破坏 ext2——因为后面的 ext2 测试套件依赖一块干净的盘,同一次 run 里不能把它写花。

> 教训:在 QEMU 上做磁盘写测试,**别假设"文件多大,盘就多大"**。以 AHCI IDENTIFY 报告的几何为准,写测试挑低位 sector + 读-改-写回。诊断这种问题时,QEMU 日志里带控制字符,`grep` 要加 `-a`(强制按文本)才匹配得到。

## 验证

```bash
# 接口 + 两个实现 + ext2 解耦,四处都在
grep -rn 'class IBlockDevice' kernel/drivers/block_device.hpp
grep -rn 'class RAMBlockDevice\|class AHCIBlockDevice' kernel/drivers/
grep -n 'explicit Ext2(cinux::drivers::IBlockDevice' kernel/fs/ext2.hpp
grep -n 'block_buf_\[' kernel/fs/ext2.hpp
```

构建 + 内核测试(这一弧 run-kernel-test 从 037 的 694 涨到 705:RAMBlockDevice +7、AHCIBlockDevice +4,ext2 解耦重构测试数不变):

```bash
cmake --build build -j$(nproc) && cmake --build build --target run-kernel-test
```

> ⚠️ 一个本弧踩到的验证坑,记给你避雷:验证构建绿时,**别在 `cmake --build` 后面接管道再读 `$?`**——那是管道最后一节(比如 `tail`)的退出码,恒为 0,会把你骗过去。要取构建真退出码,直接 `cmake --build ... > log 2>&1; echo $?`(不接管道),或 `${PIPESTATUS[0]}`。笔者这一弧就差点被这个放过一个没编译过的冲突标记。

## 小结与下一站

036-038 三章,把 F1 基建全部收口了:

- **036** Cinux-Base + ErrorOr——类型库 + 错误处理;
- **037** RingBuffer + dmesg + DMA 池——容器、日志、DMA;
- **038** IBlockDevice——块设备抽象,ext2 解耦。

到这,内核该有的公共地基都齐了:错误、容器、日志、DMA、块设备。后面 F2(内存升级)、F5(NVMe/VirtIO 驱动)、F6(文件系统升级)都站在这层之上,不用再往下重造。

下一站 **039** 离开基建弧,进 F5 驱动弧的第一步——AHCI DMA 升级(F5-M1):把 `ahci.cpp` 里剩下的手动 DMA 迁到 037 的 `DmaPool`/`PrdtBuilder`,从单 PRDT 轮询走向 scatter-gather。那是 `AHCIBlockDevice` 下面真正干活的部分。
