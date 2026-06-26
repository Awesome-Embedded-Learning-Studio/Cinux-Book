---
title: 工具链与 CMake
---

# 工具链与 CMake

> 先把"能跑起来"这条路打通:装好 GCC/CMake/QEMU,配出裸机 CMake 骨架,把 ELF 转成裸镜像拼进磁盘,最后 `make run` 让 QEMU 真正启动。本模块只讲到 Cinux 手搓 OS 用到的程度,不展开应用开发的 CMake 高级特性。

- [01 · 工具链安装与验证](01-toolchain-install.md)
- [02 · CMake 裸机骨架](02-cmake-skeleton.md)
- [03 · 目标 / 链接脚本 / ELF→裸镜像](03-targets-linker-objcopy.md)
- [04 · QEMU、磁盘镜像与主机测试](04-qemu-image-test.md)
