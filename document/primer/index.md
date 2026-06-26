---
title: 前置卷 · 在读正文之前
---

# 前置卷 · primer:在读正文 001 之前

> 正文 [001 · 实模式引导](../book/01-boot/001-boot-real-mode.md) 开篇就假设你**读得懂 AT&T 汇编、懂段/分页、会用 CMake + 链接脚本 + objcopy**。对很多人这是最大的劝退点——机器一个字节还没跑,先被一堆陌生工具和语法劝退。这一卷就是补这个缺口:横切的基础知识,不绑任何 git tag,读完你能无障碍进正文 001。

## 这卷是给谁的

- 会一点 Linux 命令行、写过简单 C/C++,**但没读过 AT&T 汇编、不懂段/分页、没用过链接脚本**的人。
- 看过一些 NASM/Intel 汇编教程,但正文 `.S` 全是 GAS/AT&T,对不上号的人。
- 想直接 `make run` 把项目跑起来,却被 CMake toolchain / objcopy / QEMU 镜像挡住的人。

如果你已经熟练这些,可以直接跳到 [正文 001](../book/01-boot/001-boot-real-mode.md),前置卷点到为止、不重复正文。

## 怎么读

建议按"先能跑、再能读、后能写"的顺序:

```text
01 工具链(能 cmake --build 出 cinux.img、make run 起黑屏)
   └─▶ 02 汇编(能逐行读懂 mbr.S / interrupts.S / context_switch.S)
        └─▶ 03 C/C++(能看懂 004 起的 C++ 内核:freestanding 子集、stub、内联汇编)
```

三大模块各自独立,不强制顺序;但**工具链先行**最省事——先把环境跑通,后面随时能 `make run` 验证想法。读完这三块,正文 001–004 的门槛就拆掉了。

## 三大模块

- **01 工具链** — 装好 GCC/CMake/QEMU、CMake 裸机骨架(toolchain file / `Generic` 关标准库)、目标与链接脚本与 objcopy、QEMU 镜像与主机测试。
  - [01 · 工具链安装与验证](01-toolchain/01-toolchain-install.md)
  - [02 · CMake 裸机骨架](01-toolchain/02-cmake-skeleton.md)
  - [03 · 目标 / 链接脚本 / ELF→裸镜像](01-toolchain/03-targets-linker-objcopy.md)
  - [04 · QEMU、磁盘镜像与主机测试](01-toolchain/04-qemu-image-test.md)
- **02 汇编** — 以 **GAS/AT&T 为本位**:语法骨架、寻址与系统指令(CR/MSR/lgdt/iretq 的 AT&T 形态)、AT&T↔Intel 速查表与编译回路。
  - [01 · GAS 语法骨架](02-assembly/01-gas-syntax-skeleton.md)
  - [02 · 寻址、远转移与系统指令](02-assembly/02-addressing-system-instr.md)
  - [03 · AT&T↔Intel 速查表与 GAS 实战](02-assembly/03-cheatsheet-and-practice.md)
- **03 C/C++** — 内核向的简短 C/C++:指针/位/布局/MMIO、freestanding C++ 子集与运行时 stub、克制用法速览。
  - [01 · C 核心:指针、位、布局与 MMIO](03-cpp/01-c-core.md)
  - [02 · freestanding C++ 子集](03-cpp/02-freestanding-cpp.md)
  - [03 · 内核里 C++ 的克制用法](03-cpp/03-restrained-cpp.md)

> 规划中的 **04 体系结构**(实/保护/长三种模式)与 **05 操作系统前置概念**(内核/ring/中断/进程地图)尚未落笔,等三模块稳定后补齐。这两块正文 001–003 会带着源码讲透,缺了它们不挡路,只是少一张"先看全景"的地图。

读完这三块,下一站就是正文 [001 · 实模式引导](../book/01-boot/001-boot-real-mode.md):让 BIOS 把你写的第一段汇编从磁盘读进 `0x7C00`,点亮黑屏。从那里起,我们写的每一行代码都会真实地在虚拟机里跑起来。
