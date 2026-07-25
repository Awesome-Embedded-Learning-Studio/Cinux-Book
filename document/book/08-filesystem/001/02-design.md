---
title: 02 · 设计图:PCI 配置空间与 AHCI 接口
---

# 设计图:PCI 配置空间与 AHCI 接口

## 设计图

整条链路分成「找设备」和「驱动设备」两段。先看 PCI 怎么认设备:

```text
   PCI 配置机制 #1:一对 I/O 口
        写地址字 ──→ 0xCF8 (CONFIG_ADDRESS)
        读写数据 ──→ 0xCFC (CONFIG_DATA)

   地址字 = bit31 使能 | bus<<16 | slot<<11 | func<<8 | (offset & 0xFC)
            └─ offset & 0xFC:寄存器偏移按 dword 对齐(低 2 位清零)

   枚举(暴力扫描 32 bus × 32 slot × 8 func):
        vendev = read(VENDOR_ID)            ← 一次读到 vendor(低16)+device(高16)
        if vendor == 0xFFFF: 这个位置没设备  ← func0 空 → 整个 slot 跳过
        class/subclass/prog_if 在 offset 0x08 那个 dword 里

   find_ahci:
        遍历,命中 class==0x01(大容量存储) && subclass==0x06(SATA/AHCI)
        read_bars:BAR5 就是 AHCI 寄存器块的物理基址(ABAR)
```

AHCI 这边,核心是「把 BAR5 映射进来 → 复位 → 给每个有设备的端口搭命令队列 → 发命令轮询」:

```text
   BAR5(物理) ──VMM.map(FLAG_PCD)──→ 内核虚拟地址(HBAMem*)
        │
        ├─ 通用寄存器:cap / ghc(全局控制) / pi(端口实现位图) / vs(版本)
        └─ ports[] @ offset 0x100,每端口 0x80 字节
                ├─ clb/clbu : Command List 基址(物理)
                ├─ fb/fbu   : FIS 接收缓冲基址(物理)
                ├─ cmd      : ST/CR/FRE/FR(引擎开关与状态)
                ├─ ssts     : SATA 状态,DET 字段==3 表示「有设备且链路通」
                ├─ ci       : Command Issue,写 1<<slot 发命令,完成时硬件清
                └─ tfd      : Task File Data,bit0 是 ERR

   发一条读命令(slot 0):
        Command List[0] ──指向──→ Command Table
                                    ├─ cfis[]:Register H2D FIS(type 0x27)
                                    │         command=0x25(READ DMA EXT)
                                    │         48位 LBA 拆进 lba0-2 / lba3-5
                                    └─ prdt[]:一个 PRD 指向目标缓冲(dbc=字节数-1)

        port.ci = 1<<0            ← 出发
        轮询 port.ci 清零 → 查 tfd.ERR==0 → 成功,数据已在缓冲
```

两段链路的衔接点是 BAR5:PCI 把它读出来交给 AHCI,AHCI 把它映射成 `HBAMem*` 后才能碰任何寄存器。
