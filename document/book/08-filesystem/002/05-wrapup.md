---
title: 05 · 收尾:下一站与参考
---

# 收尾:下一站与参考

到这里,内核第一次认出了「文件」——它能把一个 ustar 归档解析成「名字 + 大小」的列表。但你很快会发现这个抽象还太薄:它只是「列出」文件,没有「打开某个文件读它的内容」、没有路径、没有目录层级、没有统一的接口。现在内核知道 `hello.txt` 存在、有多大,但拿不到它的内容,更没法对各种来源(ramdisk、将来的磁盘文件系统)用同一套 API 操作。

下一站,我们在这层「认得文件」的基础上,搭一个虚拟文件系统(VFS):一套 `open/read/close` 的统一接口,让内核(和用户态)能用「打开一个名字、读它的内容」的方式访问文件,而不用关心文件背后是 ramdisk 还是别的什么。不过那是下一章的事,我们先把「内核能认出归档里的文件」这个里程碑坐实。

---

### 参考

- POSIX.1-1988 "ustar" 交换格式(`[ramdisk_config.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ramdisk_config.hpp)` 头注释已引):512 字节定长头布局、字段偏移(name@0、size@124、typeflag@156、magic@257)、数字字段的八进制 ASCII 编码、typeflag 字符含义(`'0'` 文件 / `'5'` 目录 / `'7'` 连续)、`"ustar\0"` magic、末尾两个全零块收尾。权威格式依据。
- Wikipedia — [tar (computing)](https://en.wikipedia.org/wiki/Tar_(computing)):ustar/POSIX 头布局与历史演变的社区参考,字段速查方便。
- GNU binutils — `objcopy`(`-I binary`、`--rename-section`、`--redefine-sym`):embed 流水线的工具依据;`-I binary` 按输入路径派生符号名是其已知行为,这正是 `embed_binary.sh` 要做重命名的原因。
- 001 章 · [让内核自己找到磁盘:PCI 枚举与 AHCI 驱动](../001/):存储前一章。本章明确说明 ramdisk 数据是构建期嵌入、**不走** AHCI 盘,两章在「数据来源」上解耦。
- 本 tag 源码:[ramdisk.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ramdisk.cpp) / [ramdisk.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ramdisk.hpp) / [ramdisk_config.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ramdisk_config.hpp)、[embed_binary.sh](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/scripts/embed_binary.sh)、[linker.ld](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/linker.ld)(`.initrd` 段)、[CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/CMakeLists.txt)(embed 流水线)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp)(Step 22);测试 [test_ramdisk.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_ramdisk.cpp)(host 镜像)、[test_ramdisk.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_ramdisk.cpp)(QEMU 真归档)。
