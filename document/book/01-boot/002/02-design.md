---
title: 02 · 设计图:扁平 GDT 与模式状态机
---

# 设计图:扁平 GDT 与模式状态机

先看这张 GDT 长什么样。它是内存里一段连续的 8 字节条目,前面三项就够我们进 PM:

```text
偏移   entry           选择子    access  base/limit        用途
0x00   [ null      ]    —        0x00    base=0,lim=0       第 0 项必须全 0
0x08   [ code      ]    0x08     0x9A    base=0,lim=4GB     32 位代码段(可执行/可读)
0x10   [ data      ]    0x10     0x92    base=0,lim=4GB     32 位数据段(可读写)
```

选择子的规则和 [002](../03-big-kernel/002/) 那张 GDT 是同一套:`选择子 = (条目偏移) | RPL`,RPL=0 不加。所以 `0x08` = 第 1 项、`0x10` = 第 2 项。

注意:这张 GDT 是 **bootloader 的最小 PM GDT,base 全是 0 的扁平模型**。别和后面 big kernel(010)那张 7 项、带 TSS、带用户段的完整 GDT 搞混——那是内核自己后来重建的。这里我们只要"刚好够进 PM"。

再看实模式 → 保护模式的**状态机**,每一步都不可逆,顺序错了就是三重故障:

```text
实模式(16 位,DS<<4+偏移)
  │  cli                 # 关中断(全程不开,没有 IDT)
  │  DS=0                # 让 lgdt 的实模式寻址算对
  │  lgdt gdt_ptr        # 把 GDT 基址/限长装进 GDTR(CPU 此刻还不校验内容)
  │  CR0.PE = 1          # 拨开关——但 CPU 还在用旧的 CS/16 位译码!
  ▼
  ljmp $0x08, $pm_entry  # ← 关键的远跳:强制用新 GDT 重载 CS,切到 32 位译码
  │
  ▼
保护模式(32 位,扁平)
  │  DS=ES=FS=GS=SS = 0x10   # 装载新数据段
  │  ESP = 0x90000           # 新栈
```
