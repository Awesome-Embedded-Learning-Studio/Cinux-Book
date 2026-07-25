---
title: 02 · 设计图:内存、磁盘、调用链
---

# 设计图:内存、磁盘、调用链

先把内存和磁盘两个布局摆出来,后面所有代码都围着它转。

**内存布局**(实模式下,`物理地址 = 段 << 4 + 偏移`):

```text
0x0000_7000   MBR 自己的栈(往下长)
0x0000_7B00   读盘用的 DAP 结构(16 字节,临时)
0x0000_7C00   MBR 代码(BIOS 读进来的第 0 扇区)
0x0000_8000   Stage2 代码(MBR 读进来的第 1+ 扇区)
0x0000_9000   Stage2 的栈基址(SS=0x900,往下长到 0xFFFE 之下)
0x0000_6000   VBE Controller Info(BIOS 写)
0x0000_6200   VBE Mode Info(BIOS 写)
0x0000_6400   我们保存的 framebuffer 信息(留给将来内核)
```

**磁盘布局**:

```text
扇区 0       MBR(512B,末尾 0xAA55)
扇区 1..15   Stage2(最多 7.5KB,15 扇区)
```

**调用链**——整章就这一条主路:

```text
BIOS
 └─▶ MBR _start @ 0x7C00
      └─▶ ljmp $0,$real_start        # 先把 CS 归零,理顺段
           └─▶ real_start: 设栈、存 dl、
                └─▶ load_stage2()    # INT 0x13 AH=0x42 读盘到 0x8000
                └─▶ ljmp $0x800,$0   # 远跳到 Stage2
                     └─▶ Stage2 _start @ 0x8000
                          ├─ 重置段/栈
                          ├─ enable_a20()       # INT 0x15 AX=0x2401
                          ├─ vesa_get_controller_info()  # INT 0x10 AX=0x4F00
                          ├─ vesa_get_mode_info()        # INT 0x10 AX=0x4F01, mode 0x118
                          ├─ vesa_set_mode()             # INT 0x10 AX=0x4F02, 0x4118
                          ├─ vesa_save_framebuffer_info()# 存到 0x6400
                          └─ hlt 循环
```
