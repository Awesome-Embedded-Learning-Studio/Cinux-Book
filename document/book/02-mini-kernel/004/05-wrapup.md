---
title: 05 · 验证与下一站
---

# 验证与下一站

这一章的测试是整个 mini-kernel 卷里最重的,因为 ATA 命令编码、ELF 头解析全是纯逻辑,特别适合 host 单测。

host 单测(主力):

```bash
cmake --build build --target test_host
```

`test_ata`(五百多行)测命令编码和状态解析——给定 LBA/扇区数,验写进各寄存器的字节对不对、LBA28/48 的选择对不对;`test_elf_loader`(七百多行)测头解析和段加载——构造各种 ELF 头(合法的、magic 错的、class 错的)、各种 PT_LOAD 段(带 BSS 的、越界的),验加载行为;`test_big_kernel_loader` 测编排。这些都在 host 上跑,快且能在 CI 里反复磨。

QEMU 内核测试:

```bash
cmake --build build --target run-kernel-test
```

`test_ata`、`test_elf_loader` 在真内核里跑,验它们在真硬件(QEMU 的 ATA 控制器)上行为正确。

量产看 demo:

```bash
cmake --build build --target run
```

串口依次出现 `[INIT] ATA controller initialized`、`[DEMO] Reading MBR (LBA 0)...`、`MBR boot signature: 0xaa55 (VALID)`、`Reading mini kernel header (LBA 16)...`、`No valid ELF header at LBA 16 (expected for flat binary)`、最后 `Milestone 008 complete. Waiting for big kernel (009+)...`。`0xAA55 (VALID)` 和"expected for flat binary"这两行,就是这套加载流水线能干的铁证。

## 下一站

mini kernel 现在什么都备齐了:会输出、有内存、能扛异常、还能读盘解析 ELF。唯一缺的,是那个要被加载的主角——big kernel。这一章我们造好了枪、校好了准星,但靶子还没竖起来。

下一章 [001 · 大内核入口](../03-big-kernel/001/),big kernel 终于登场。它会被写进磁盘 LBA 848 之后,mini kernel 用这一章造好的 `load_big_kernel()` 把它读进来、加载好、跳进它的入口。那一刻,mini kernel 完成它全部的接力使命,把舞台正式交给功能完整的 big kernel——从那以后,Cinux 的故事就是 big kernel 的故事了。

---

### 参考

- OSDev — [ATA PIO Mode](https://wiki.osdev.org/ATA_PIO_Mode)(I/O 端口、状态位 BSY/DRQ/RDY/ERR、LBA28/LBA48、400ns 延时、READ SECTORS 命令)、[ELF](https://wiki.osdev.org/ELF)(Elf64_Ehdr/Elf64_Phdr、PT_LOAD、p_filesz/p_memsz 与 BSS)。
- Tool Interface Standard (TIS) ELF 规范 — ELF64 文件头与程序头字段定义、e_entry、PT_LOAD 段语义。
- 本 tag 源码:[ata.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/driver/ata.cpp)/[ata.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/driver/ata.hpp)、[elf_loader.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/elf_loader.hpp)/[elf_loader.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/elf_loader.cpp)、[big_kernel_loader.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/big_kernel_loader.cpp)/[big_kernel_loader.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/big_kernel_loader.hpp)、[string.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/lib/string.cpp)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/main.cpp)、[test_ata.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_ata.cpp)/[test_elf_loader.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_elf_loader.cpp)/[test_big_kernel_loader.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_big_kernel_loader.cpp)。
