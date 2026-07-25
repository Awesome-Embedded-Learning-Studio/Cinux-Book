---
title: 02 · 设计图:磁盘布局、PIO 时序、ELF 流水线
---

# 设计图:磁盘布局、PIO 时序、ELF 流水线

先看磁盘长什么样。到现在,盘上已经住了三房客:MBR、stage2、mini kernel;big kernel 预订在更后面:

```text
扇区 0        MBR(512B,末尾 0xAA55)
扇区 1..15    Stage2
扇区 16..847  mini kernel(832 扇区 ≈ 416KB)
扇区 848+     big kernel(预订,009 才入住)
```

`big_kernel_loader` 的常量就是照这个布局定的:`BIG_KERNEL_LBA = 848`、`BIG_KERNEL_MAX_SECTORS = 512`(256KB 上界,够装任何合理的内核)。

再看 ATA 读一个扇区的 PIO 时序,精髓是"下命令 → 轮询状态 → 搬数据":

```text
read(lba, count, buf):
  选 master + LBA 模式,把 lba 拆字节写进 LBA_LOW/MID/HIGH 寄存器
  写 SECTOR_CNT = count
  写 COMMAND = READ_PIO(0x20)         ← 下命令
  for 每个扇区:
    delay 400ns                        ← ATA 规范要求,读控制口 4 次
    wait_data_ready()                  ← 轮询:BSY 清零 且 DRQ 置位
    for 256 次: buf[word] = inw(DATA)  ← 从 16 位数据端口搬 512 字节
```

最后是 ELF 加载这条流水线:

```text
big_kernel → staging@0x1000000(16MB,ATA 读来的原始 ELF)
   └─ load_elf:
        验 ELF magic(7F 45 4C 46)
        遍历 program header:
          PT_LOAD 段 → 从 p_offset 拷 p_filesz 字节到 p_paddr
                       再把 p_memsz - p_filesz 清零(BSS)
        返回 e_entry(入口地址)
```
