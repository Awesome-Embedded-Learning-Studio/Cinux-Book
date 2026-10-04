---
title: 工具链
---

# 工具链

> 手搓系统的人,一半的功夫花在工具上。这一卷咱们把家什一件件装齐、点亮,再看看 g++ 背后指挥了谁,把 host、target、freestanding 三个词认成手里的开关,最后亲手链出一枚 BIOS 认的裸镜像。

- [第 01 篇 · 装什么](01-what-to-install.md) — 装一件,点亮一件,过两道闸
- [第 02 篇 · 编译驱动解剖](02-anatomy-of-gxx.md) — 一条 g++ 命令背后的四道工序
- [第 03 篇 · 目标三重奏](03-host-target-freestanding.md) — host、target 与 freestanding 三个词
- [第 04 篇 · 链接脚本与裸镜像](04-linker-flat-image.md) — 从 .o 到一枚 512 字节的镜像
- [第 05 篇 · ELF 深读](05-elf.md) — 三张纸:文件头、节表、程序头表
- [第 06 篇 · QEMU 镜像与真跑](06-qemu.md) — 把镜像装进模拟器,看它真的动起来
- [第 07 篇 · GDB 回路](07-gdb.md) — 条件断点、脚本,和那个改一行看一圈的回路
