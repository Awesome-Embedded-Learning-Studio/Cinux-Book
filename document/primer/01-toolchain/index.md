---
title: 工具链与 CMake
---

# 工具链与 CMake

> 四章回答一路的四个问题:咱们怎么把它跑起来?这套工具凭什么能编出内核?代码怎么变成 BIOS 认的裸二进制?跑完又怎么知道它对了?按顺序走完,`make run` 背后的每一环咱们都亲手摸过一遍。

- [01 · 装机与首胜](01-toolchain-install.md) — 三十分钟,从 apt 到 QEMU 黑屏
- [02 · 工具链第一课](02-compiler-first-lesson.md) — 交叉编译、freestanding,与那串 flag 的逐条为什么
- [03 · 目标 / 链接脚本 / 裸镜像](03-targets-linker-objcopy.md) — 把代码"摆"到它该在的地址
- [04 · QEMU、磁盘镜像与主机测试](04-qemu-image-test.md) — 跑起来,并让内核自报成败
