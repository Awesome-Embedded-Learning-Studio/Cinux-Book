---
title: 02 · 设计图:磁盘、内存、BootInfo 交接
---

# 设计图:磁盘、内存、BootInfo 交接

先看磁盘和内存两个布局。

**磁盘布局**(比 001 多了一段内核):

```text
扇区 0       MBR(512B)
扇区 1..15   Stage2(≤7.5KB)
扇区 16+     mini kernel ELF(416KB,832 扇区)
```

**内存布局**(004 新增/用到的关键地址):

```text
0x5000   E820 内存图(query_memory_map 写入)
0x6400   VESA 帧缓冲信息(001 存的)
0x7000   BootInfo 交接结构(824 字节,bootloader 填、内核读)
0x20000  mini kernel 物理载入地址(LMA)
0x90000  保护模式/长模式栈(内核加载要避开它——见调试现场)
0xFFFFFFFF80020000  mini kernel 虚拟运行地址(VMA,高半)
```

**调用链与交接**:

```text
bootloader(实模式): 读盘 → 内存图
   ↓
long_mode_entry(64 位): 填 BootInfo@0x7000 → rdi=0x7000 → jmp 高半入口
   ↓                          ↑ rdi 传参(System V AMD64 ABI)
kernel _start: 存 boot_info → 清 BSS → 全局构造 → main(BootInfo*)
```
