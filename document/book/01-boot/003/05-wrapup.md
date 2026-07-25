---
title: 05 · 验证、下一站与参考
---

# 验证、下一站与参考

## 验证

第一道闸还是构建。老规矩——003 没有运行时自动化测试,构建本身就是冒烟:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -S .
cmake --build build -j$(nproc)
```

`stage2.bin` 里现在嵌了 `.code64` 段,能产出说明汇编器接受了 16/32/64 位混合编码。

第二道闸看 debugcon。`cmake --build build --target run`,跑完看:

```bash
cat build/debug.log    # 期望:PL
```

`P` 是 002 进 PM 时打的、`L` 是本章进长模式时打的。两个都在,说明从实模式一路走到 64 位长模式全程没崩。少了 `P` 或 `L`、或者出现乱码,就照"调试现场"对号入座。

第三道闸用 GDB 确认模式。`cmake --build build --target run-debug`,另一终端:

```text
(gdb) file build/boot/stage2
(gdb) target remote :1234
(gdb) b *long_mode_entry
(gdb) c
# 命中断点 = 远跳成功
(gdb) p/x $cs                         # 应是 0x18(64 位代码段)
(gdb) monitor info registers           # 或看 EFER.LMA 位、CR0.PG 位
```

能停在 `long_mode_entry`、`cs=0x18`、EFER 的 LMA(Long Mode Active)位为 1,就是实打实的 64 位。

## 下一站

现在 Cinux 是一个货真价实的 64 位长模式环境了:有分页、有 64 位寄存器、有一个能跑的栈。可它还停在 bootloader 里 `hlt`——我们还没真正"启动一个内核"。长模式只是把舞台搭好,真正的主角(C++ 写的内核)还没登场。

下一章 [004 · 加载 mini kernel](../004/),我们要让 bootloader 把第一个用 C++ 写的内核镜像从磁盘读进来,跳进它的入口,让真正的"内核代码"第一次跑起来。从那以后,汇编 bootloader 的使命就基本完成,接力棒交给 C++。

---

### 参考

- Intel SDM Vol.3A — §4.1 四级分页(PML4/PDPT/PD/PT 结构)、§4.3 2MB/4MB 大页(PS 位)、§4.5 PAE、§11.8.2 EFER 与长模式使能(LME/MSR `0xC0000080`)、§9.8.1.1 切换到长模式的固定序列(`CR3`→`CR4.PAE`→`EFER.LME`→`CR0.PG`→远跳)。
- OSDev — [Setting Up Long Mode](https://wiki.osdev.org/Setting_Up_Long_Mode)(进入序列与临时恒等映射)、[Page Tables](https://wiki.osdev.org/Page_Table)(四级结构与页表项标志位)、[Creating a 64-bit kernel](https://wiki.osdev.org/Creating_a_64-bit_kernel)(64 位 GDT 的 L 位要求)。
- 本 tag 源码:[long_mode.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/common/long_mode.S)(`setup_page_tables`、`enter_long_mode`)、[stage2.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/stage2.S)(`.code64 long_mode_entry`、扩展 5 项 GDT + `gdt64_ptr`)、[CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/CMakeLists.txt)(`boot_longmode` 对象库)。
- 调试素材提炼自 [1.md](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/document/notes/003/1.md)。

> Intel SDM 版本说明:本卷引用的 SDM 章节号沿用较早版本编号。若按项目本地 PDF(`document/reference/intel/`,2023-06 版)查阅,部分内容已重排——四级分页在 §4.5、2MB 大页见 §4.5、PAE 在 §4.4、EFER 在 §2.2.1、切换到长模式在 Chapter 10。以章节标题为准,别拘泥于编号。
