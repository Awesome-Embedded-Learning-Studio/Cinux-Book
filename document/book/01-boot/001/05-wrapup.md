---
title: 05 · 验证、下一站与参考
---

# 验证、下一站与参考

## 验证

先说清楚:**001 没有 host 侧的自动化测试**。这一阶段的 fact-lock 里,所谓"测试"只有 `boot/CMakeLists.txt` 本身——也就是说,**能构建出 `mbr.bin` / `stage2.bin` / `cinux.img`,就算汇编、链接、objcopy、磁盘拼装这一路全过了**。这是第一道闸:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -S .
cmake --build build -j$(nproc)
```

构建产物里 `build/boot/mbr.bin` 必须是 512 字节,`scripts/build_image.sh` 还会校验它的末两字节是不是 `55 aa`(魔数 `0xAA55`)。这一步没过,后面都白搭。

跑起来:

```bash
cmake --build build --target run     # 或 cd build && make run
```

这里有个**容易误判**的地方:001 里所有打印都走 `INT 0x10 AH=0x0E`(VGA teletype),它写到的是 **VGA 文本模式**——也就是 QEMU 的图形**窗口**里,而**不是** `-serial stdio` 那个串口终端。所以别盯着命令行的 stdout 看,那里什么都没有;去 QEMU 弹出的窗口里看。正常你会按顺序看到:

```text
Cinux Booting...
Stage2 OK
Mode info OK, switching...
```

然后屏幕"啪"地一切——VESA 设模式成功,文本模式没了,窗口变黑(因为还没人往 framebuffer 画东西),机器安静停住。看到这个,001 就成了。

要是 VESA 三步里有一步 BIOS 返回失败(`AL != 0x4F`),代码会 `jmp panic` 打印对应的错误串(`VESA: Controller info failed!` 之类)然后 `hlt`——这能帮你定位是哪一步挂了。

想确认 framebuffer 存档真的写对了,可以挂 GDB 看一眼 `0x6400`(`make run-debug` 起带 `-s -S` 的 QEMU,另一个终端 `gdb` 连 `:1234`):

```text
(gdb) target remote :1234
(gdb) x/2gx 0x6400
0x6400: 0x00000000fd000000 0x......    # 前 8 字节是显存物理地址(如 0xfd000000)
```

低 8 字节是物理地址,后面跟着 pitch、宽、高。能读到一个合理的物理地址(典型如 `0xfd000000` 附近的显存区),就说明 VESA 这一路真走通了,不只是"没崩"。

## 下一站

到这里,我们的机器会读盘、会切屏、栈也稳了,framebuffer 参数也替将来的内核存好了。可我们一直窝在实模式里——1MB 寻址上限、段式地址这套别扭的寻址、没有任何内存保护。

下一章 [002 · 进入保护模式](../002/),我们要从实模式跳出去:建一张 GDT,把 `CR0` 的保护使能位打开,让 CPU 进入 32 位保护模式。这一跳之后,BIOS 就再也用不了了——所以你看,这一章里我们拼命把"要用 BIOS 的活"提前干完,就是为了这个离别的时刻做准备。

---

### 参考

- Intel SDM Vol.3A — §3.2/§3.3 实模式与段式地址翻译(`物理地址 = 段 << 4 + 偏移`)、§9.1.4 处理器上电/复位后的初始状态(实模式入口)。
- OSDev — [Boot Sequence](https://wiki.osdev.org/Boot_Sequence)(BIOS 加载第一个可引导扇区到 `0x7C00` 并跳转)、[Memory Map (x86)](https://wiki.osdev.org/Memory_Map_(x86))(`0x7C00` 约定)、[A20 Line](https://wiki.osdev.org/A20_Line)(为什么需要、`INT 0x15 AX=0x2401`)、[VESA Video Modes](https://wiki.osdev.org/VESA_Video_Modes)(VBE `0x4F00/0x4F01/0x4F02` 与线性 framebuffer 位)。
- Ralf Brown's Interrupt List — `INT 0x13 AH=0x42`(扩展读、DAP 布局)、`INT 0x10 AX=0x4F0x`(VBE):http://www.ctyme.com/intr/。
- 本 tag 源码:[mbr.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/mbr.S)、[stage2.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/stage2.S)、[serial.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/common/serial.S)、[CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/CMakeLists.txt)、[build_image.sh](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/scripts/build_image.sh)、[qemu.cmake](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/cmake/qemu.cmake)。
- 调试素材提炼自 [notes_mbr.md](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/document/notes/001/notes_mbr.md) 与 [note2_check_framebuffer.md](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/document/notes/001/note2_check_framebuffer.md)。

> Intel SDM 版本说明:本卷引用的 SDM 章节号沿用较早版本编号。若按项目本地 PDF(`document/reference/intel/`,2023-06 版)查阅,部分内容已重排——实模式地址翻译在 §21.1.1、复位/上电入口在 §10.1.4、模式切换在 Chapter 10、控制寄存器(CR0/CR4)在 §2.5。以章节标题为准,别拘泥于编号。
