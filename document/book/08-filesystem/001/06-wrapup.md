---
title: 06 · 收尾:下一站与参考
---

# 收尾:下一站与参考

## 下一站

到这里,内核第一次能自己认出磁盘、自己读写扇区了。但你会发现一个抽象层的缺口:我们读出来的是**裸扇区**——512 字节一坨,没有「文件」、没有「目录」、没有名字。现在内核会读盘了,但还不知道盘上那一堆扇区该怎么组织成「文件」。

下一站,我们先从最简单的开始:把一张预先打包好的镜像(initrd)当内存盘,在扇区上叠一层最小的「文件」抽象——按某种布局读出「哪个文件在第几扇区、多长」。这能在没有完整文件系统、也不依赖磁盘写操作的前提下,让内核加载并运行磁盘上的程序。不过那是下一章的事,我们先把「内核能自己找到磁盘并读出扇区」这个里程碑坐实。

---

### 参考

- Intel AHCI Specification rev 1.3(`[ahci_config.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/ahci/ahci_config.hpp)` 头注释已引):HBA 寄存器布局(通用区到 `0x100`、每端口 `0x80`)、`pi` 端口位图、`GHC.AE/HR/IE`、`PxCmd.ST/CR/FRE/FR` 的端口状态机与停起顺序、`PxSSTS.DET`(`0x03`=设备在线)、Command List(32×32B)/Received FIS(256B)的尺寸与对齐、Register H2D FIS(`0x27`)字段、PRDT 的 dbc「字节数-1」约定。权威硬件依据。
- OSDev — [PCI](https://wiki.osdev.org/PCI):配置机制 #1(`0xCF8`/`0xCFC`、bit31 使能)、配置空间寄存器偏移、BAR 类型(IO/内存、32/64 位)解码、class code 表。
- OSDev — [AHCI](https://wiki.osdev.org/AHCI):ABAR=BAR5、命令提交与 CI 轮询的社区实现路线、端口初始化步骤,是这套驱动最直接的对照参考。
- 016 章 · [把物理页挂进虚拟地址:VMM](../05-memory/002/):BAR5 的 MMIO 映射、命令列表/FIS 页的「物理直访」,直接复用 016 的 `g_vmm.map`、`FLAG_PCD` 和 `phys + KERNEL_VMA` 高半区约定。
- 本 tag 源码:[pci.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/pci/pci.cpp) / [pci.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/pci/pci.hpp) / [pci_config.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/pci/pci_config.hpp)、[ahci.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/ahci/ahci.cpp) / [ahci.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/ahci/ahci.hpp) / [ahci_config.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/ahci/ahci_config.hpp)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp)(Step 20-22 集成);测试 [test_ahci.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_ahci.cpp)(host 镜像)、[test_ahci.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_ahci.cpp)(QEMU 真跑)、[create_ahci_test_disk.sh](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/scripts/create_ahci_test_disk.sh)(造测试盘)、[qemu.cmake](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/cmake/qemu.cmake)(挂 AHCI 盘)。
