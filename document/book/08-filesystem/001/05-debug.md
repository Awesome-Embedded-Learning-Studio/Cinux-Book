---
title: 05 · 调试现场与验证
---

# 调试现场与验证

## 调试现场

001 这个 tag 没有独立的 notes 文件,但 PCI + AHCI 是典型「写错一个 flag 就整条链路静默失败」的硬件驱动,有几个坑值得当成调试现场(参 016 的先例:没 notes 也从代码隐患推导)。

一是 **PCI 的 Bus Master / Memory Space 没开**。`init` 里那两行 `cmd_reg |= (1<<1) | (1<<2)` 不是装饰。Bus Master 位不开,控制器不能发起 DMA,命令发出去石沉大海、`ci` 永不清零;Memory Space 位不开,MMIO 访问读到的是全 0 或全 FF,连 `pi` 位图都读不对。症状是「PCI 能认到 AHCI、BAR5 地址也对,但一碰寄存器全是垃圾」。根因就是忘了往 COMMAND 寄存器写这两位——很多教程默认 BIOS 已经开好了,但内核复位过 HBA 后得自己确保。

二是 **MMIO 漏了 `FLAG_PCD`**。`map_bar5` 的 flag 里 `FLAG_PCD` 是命门。漏了它,CPU 会把 BAR5 那块寄存器当普通内存缓存起来。于是你读 `port->ci` 读到的是进循环前缓存的「1」,死活等不到它清零,最后 POLL_TIMEOUT 超时;或者你以为发了复位命令,其实只进了 CPU 缓存、控制器压根没收到。这种「读到的值不随设备变化」、轮询必超时的症状,九成是 MMIO 被缓存了。规矩:凡是映射设备寄存器,flag 必带 `FLAG_PCD`(设备型内存不可缓存)。

三是 **端口停起顺序写反**。`stop_port` 必须「先 ST→等 CR 清→再 FRE→等 FR 清」,`start_port` 必须「先 FRE→再 ST」。这套顺序是 AHCI 规范规定的端口状态机要求。顺序反了(比如起机时先 ST 后 FRE),控制器会拒绝启动命令引擎,`setup_port` 看着成功了、`ci` 也写了,但命令永远不执行、`ci` 不清。改 `clb/fb` 前没停机就更糟——引擎还在跑,你换了它读命令的地址,数据写到不可预测的地方。

四是 **直接拿物理地址当虚拟地址访问的脆弱性**。命令列表页和 FIS 页,代码用 `phys + 0xFFFFFFFF80000000ULL`(KERNEL_VMA)直接清零、写命令头。这能工作,完全依赖「整段物理内存都做了高半区恒等映射」这个 boot 期约定(016 的 `phys_to_virt` 同理)。一旦以后缩小高半区映射范围,或某页物理内存落在没映射的区段,这行就会缺页——而此刻可能正在处理别的缺页,递归下去 double fault。所以心里要有数:这章对 PMM 给的页做「物理直访」,是借了高半区映射的光,不是放之四海皆准的做法。

## 验证

验证分两层,和 016 一样:纯逻辑在 host 上镜像测,真硬件交互在 QEMU 里跑。

host 单测 [test_ahci.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_ahci.cpp) 把能脱离硬件测的逻辑抄了一份——结构体大小(`HBAPort` 必须 `0x80`、`HBAMem` 减一个端口必须 `0x100`、命令头 `32`、PRD `16`、FIS `20`)、各类常量位定义、CFIS 的 48 位 LBA 与 count 编码、PCI 地址字构造、BAR 的 IO/32/64 判别、PRDT 字节数计算。因为 `ahci.cpp` 里全是 MMIO + 真物理内存操作,host 上没法直接调内核实现,所以测的是「同样的算法在 host 上对不对」:

```bash
ctest --test-dir build -R ahci --output-on-failure
```

「真寄存器、真 DMA」只有 QEMU 里验得了真。先把测试盘造出来——[create_ahci_test_disk.sh](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/scripts/create_ahci_test_disk.sh) 拿 `dd` 造一张 1MB 全零盘,在偏移 510、511 处写 `0x55`、`0xAA`。QEMU 那边 [qemu.cmake](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/cmake/qemu.cmake) 给测试目标挂上 `-device ahci,id=ahci -drive file=ahci_test.img,if=none,id=ahci-disk -device ide-hd,drive=ahci-disk,bus=ahci.0`,于是 QEMU 里就有一块挂在 AHCI 总线上的 SATA 盘。

机内集成测试 [test_ahci.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_ahci.cpp) 三步走:PCI 能找到 AHCI(`class==0x01 && subclass==0x06` 且 BAR5 非 0)、`AHCI::init` 后 `hba_mem()` 非空、读扇区 0 后 `buf[510]==0x55 && buf[511]==0xAA`。跑它:

```bash
cmake --build build --target run-kernel-test
```

或者直接跑完整内核看启动日志,会打出一串 `[PCI]`、`[AHCI]` 行,最后那句就是验收点:

```text
[AHCI] Read sector 0: 55 AA
```

看到 `55 AA`,说明从「枚举认设备」到「DMA 收数据」整条链路通了——这一章就成了。验证的难点和 016 类似:AHCI 的正确性没法直接「看」,只能靠「读出的数据对不对」间接验证,所以那批焊死布局与编码的 host 单测(卡住结构体/常量)+ 机内测(真跑一遍 DMA)缺一不可。
