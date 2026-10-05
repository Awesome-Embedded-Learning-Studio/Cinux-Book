---
title: 前置卷 · 在读正文之前
---

# 前置卷 · primer

> 咱们要手搓一个操作系统,门槛却大半在机器外面:书房怎么收拾,工具链怎么装,汇编与 C++ 在裸地上的用法,还有命令行那条线。这几卷把家什一件件备齐,读完您就能顺当地进[正文](../journey/)——那边,代码是咱们一行一行亲手搓出来的。

## 现在有什么

- **〇 把书房准备好** — 把 Linux 请进 Windows,认下终端、文件、包管理器和编辑器这几样日常家什
  - [01 · 把 Linux 请进 Windows](00-workshop/01-why-linux-study.md)
  - [02 · 终端生存十条](00-workshop/02-terminal-survival.md)
  - [03 · 文件的世界观](00-workshop/03-file-worldview.md)
  - [04 · 包管理器](00-workshop/04-package-manager.md)
  - [05 · 编辑器最小生存](00-workshop/05-editor-minimal.md)
- **01 工具链** — 装齐、点亮、看看 g++ 背后的工序、认下三个词、亲手链出裸镜像
  - [01 · 装什么](01-toolchain/01-what-to-install.md)
  - [02 · 编译驱动解剖](01-toolchain/02-anatomy-of-gxx.md)
  - [03 · 目标三重奏](01-toolchain/03-host-target-freestanding.md)
  - [04 · 链接脚本与裸镜像](01-toolchain/04-linker-flat-image.md)
  - [05 · ELF 深读](01-toolchain/05-elf.md)
  - [06 · QEMU 镜像与真跑](01-toolchain/06-qemu.md)
  - [07 · GDB 回路](01-toolchain/07-gdb.md)
- **02 汇编** — AT&T 为本位,从认脸读到能通读真码,再把那几行非汇编不可的指令塞进 C++
  - [01 · 为什么汇编劝退人,以及只需要学多少](02-assembly/01-why-assembly.md)
  - [02 · 读:AT&T 语法最小集](02-assembly/02-read-att-syntax.md)
  - [03 · 标号、跳转与 16 位初见](02-assembly/03-labels-and-jumps.md)
  - [04 · 栈与返回地址](02-assembly/04-stack-and-return.md)
  - [05 · 函数的门面](02-assembly/05-function-facade.md)
  - [06 · 写:内联汇编](02-assembly/06-write-inline-assembly.md)
- **03 C 与现代 C++** — 裸地上写 C++ 还剩什么,哪些惯用法在无标准库时还成立
  - [01 · 字节得自己数](03-cpp/01-bytes.md)
  - [02 · 裸地上 C++ 还剩什么](03-cpp/02-freestanding.md)
  - [03 · 惯用法识别卡与零开销复算](03-cpp/03-cards.md)
- **04 计算机怎么工作** — 顺着按下电源之后机器发生了什么这条线索,走四层:上电取指、段寻址、保护模式、设备
  - [01 · 上电与取指](04-computer/01-power-on.md)
  - [02 · 实模式与段寻址](04-computer/02-segments.md)
  - [03 · 保护模式与长模式预览](04-computer/03-protected.md)
  - [04 · 设备怎么碰](04-computer/04-devices.md)
- **05 OS 全景** — 一张图:内核是什么,进程、调度、内存、文件系统四张脸,外加一点并发的直觉
  - [01 · 内核是什么](05-os/01-what-is-kernel.md)
  - [02 · 四象限导览](05-os/02-four-quadrants.md)
  - [03 · 并发直觉](05-os/03-concurrency.md)
- **06 Unix 使用侧** — 把命令行这条线走一遍:shell 与 fd、管道与重定向、进程、信号、终端、套接字
  - [00 · 开卷总述](06-unix/00-overview.md)
  - [01 · shell 与文件描述符](06-unix/01-shell-fd.md)
  - [02 · 管道与重定向](06-unix/02-pipe-redirect.md)
  - [03 · 进程生命周期](06-unix/03-process-lifecycle.md)
  - [04 · 信号](06-unix/04-signals.md)
  - [05 · 终端与伪终端](06-unix/05-tty-pty.md)
  - [06 · 套接字](06-unix/06-sockets.md)

## 怎么读

顺序上咱们不强求:卷与卷之间随您挑。书房与工具链这两卷您读到位了,进[正文](../journey/)的第一个卷就顺了。剩下的几卷缺哪一块,您回头翻哪一卷。您想往更深处去的,隔壁还有一层[教科书](../textbook/)在慢慢长着,正文的机制讲到哪儿、您就能在那儿读到哪儿。
