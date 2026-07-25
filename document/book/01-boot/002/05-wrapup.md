---
title: 05 · 验证、下一站与参考
---

# 验证、下一站与参考

## 验证

第一道闸是构建。和 001 一样,002 没有 host 侧自动化测试,构建本身就是冒烟:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -S .
cmake --build build -j$(nproc)
```

`build/boot/stage2.bin` 能产出、`cinux.img` 能拼好,就过。

第二道闸是跑起来分两段看。`cmake --build build --target run`:

- **切 PM 之前**(还在实模式):看 QEMU 窗口,001 那几行文本(`Stage2 OK`、`Mode info OK, switching...`)照常出现,屏幕切进图形模式。这段和 001 完全一样。
- **切 PM 之后**(没 BIOS、屏幕是图形):没有任何屏幕/串口输出。这时候去看 **`build/debug.log`**——里面应该有一个 `P`(我们 `outb` 到 `0xE9` 的)。有 `P`,就证明 `pm_entry` 执行到了,保护模式切换成功。

```bash
cat build/debug.log    # 期望看到 'P'(可能还有点尾部噪声)
```

第三道闸是用 GDB 确认模式真的切了。`cmake --build build --target run-debug` 起带 `-s -S` 的 QEMU,另一终端:

```text
(gdb) file build/boot/stage2          # 用 ELF,别用 bin
(gdb) target remote :1234
(gdb) b *pm_entry
(gdb) continue
# 命中断点说明 far jump 成功;此时 info registers 应是 32 位(eip/eax 等)
(gdb) info registers eflags           # 看 VM/RF 位,确认已不在 V8086/实模式
```

能停在 `pm_entry`、寄存器名从 `ip` 变 `eip`,就是实打实地进了 32 位保护模式。

## 下一站

我们现在是 32 位保护模式,有一张扁平 GDT、一个能跑的栈、一个 debugcon 后门。可 x86_64 的故事在 64 位——32 位 PM 只是个中转站。要进 64 位长模式,得先建一套**分页**(因为长模式强制要求分页开启),把 `CR4.PAE`、`EFER.LME`、`CR0.PG` 一个个拨起来,然后再来一次远跳,带着一个 L 位=1 的 64 位代码段选择子。

下一章 [003 · 长模式](../003/),我们就在这张 32 位 PM 的地基上,把分页和长模式搭起来,让 Cinux 真正变成 64 位。那张 64 位的 GDT,以及后面 big kernel 自己重建的完整 GDT,都是后话——现在我们只需要这刚刚够用的三行。

---

### 参考

- Intel SDM Vol.3A — §3.4.2 Segment Descriptors(描述符格式与位定义)、§3.4.4 GDTR/LGDT、§3.5 控制寄存器(CR0 与 PE 位)、§9.9 Switching to Protected Mode(标准步骤、far jump 要求、`cli` 时机)。
- OSDev — [Global Descriptor Table](https://wiki.osdev.org/Global_Descriptor_Table)(扁平模型的最小 GDT)、[Protected Mode](https://wiki.osdev.org/Protected_Mode)。
- OSDev — [Debugcon](https://wiki.osdev.org/Debugcon)(QEMU 端口 `0xE9` 调试后门)。
- 本 tag 源码:[stage2.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/stage2.S)(PM 切换序列与 GDT)、[serial.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/common/serial.S)(pushw 化)、[CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/CMakeLists.txt)(链接地址 `0x8000`、`.gdt` 段)、[qemu.cmake](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/cmake/qemu.cmake)(debugcon)。
- 调试素材提炼自 [1.md](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/document/notes/002/1.md) 与 [2.md](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/document/notes/002/2.md)。

> Intel SDM 版本说明:本卷引用的 SDM 章节号沿用较早版本编号。若按项目本地 PDF(`document/reference/intel/`,2023-06 版)查阅,部分内容已重排——段描述符在 §3.4.5、GDTR/LGDT 在 §2.4.1、控制寄存器(CR0/PE)在 §2.5、切换到保护模式在 §10.9。以章节标题为准,别拘泥于编号。
