---
title: 002 · 内核第一次认识「文件」:嵌入式 initrd ramdisk
---

# 002 · 内核第一次认识「文件」:嵌入式 initrd ramdisk

> 从「扇区」走到「文件」:让内核解析一个 ustar 格式的归档,把里面每个文件的名字和大小列出来。归档构建期嵌进内核镜像,先把「文件」这个抽象立起来。

## 本章路线

- [01 · 导引:点亮什么与为什么](01-intro.md)
- [02 · 设计图:ustar 与 initrd](02-design.md)
- [03 · 实现:ustar 头、八进制、mount、embed 流水线](03-implementation.md)
- [04 · 调试现场与验证](04-debug.md)
- [05 · 收尾:下一站与参考](05-wrapup.md)
