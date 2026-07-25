---
title: 03 · 调试现场、验证与下一站
---

# 调试现场、验证与下一站

## 调试现场

这一章留下一个特别精彩的 bug,记录在 [009-01-elf-loader-header-corruption.md](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/document/notes/009/009-01-elf-loader-header-corruption.md)。

症状是:`test_big_kernel_load` 跑到打印 `PT_LOAD[0]` 信息后突然中断,QEMU 直接退出,连个异常都没有,后面所有测试消失。崩溃点卡在 ELF 加载的"打印段信息"和"Loaded segment"两行之间——也就是 `load_elf` 拷段的那一刻。

根因是**加载器把自己正在读的 ELF 头覆盖了**。big kernel 的 staging 缓冲区在 `0x1000000`,而它的 `PT_LOAD[0].p_paddr` 也是 `0x1000000`——同一个地址。`load_elf` 一边从 staging 里读 ELF 头和 program header(在镜像头部),一边把段数据 `memcpy` 到 `p_paddr`(也就是 staging 起点)。这一拷,段数据直接盖到了 ELF 头上;循环到下一段再去读 program header,读到的已经是段数据的垃圾,当场崩。

这是个"load-in-place"布局下的经典陷阱:当目标物理地址和 staging 地址重合,`memcpy` 的源和目的区域会重叠,既不能用 `memcpy`(重叠区是未定义行为),又不能用普通的"边读头边加载"。Cinux 的修法分两步:其一,`load_elf` 在动数据前先把 program header 整组**快照到一个本地数组**(`saved_phdrs`),后续循环读的是快照、不再碰 staging 里会被覆盖的那份;其二,段拷贝改用 `memmove`(重叠区安全)而不是 `memcpy`。再加上前面说的运行时 `check_memory_overlaps` 和构建期 `check_memory_layout.py` 把致命重叠挡在加载前。这个 bug 现在是"过去时",但它的教训——**加载器的源区(staging)和目的区(p_paddr)一旦可能重叠,就得先快照元数据、再用 memmove**——值得记一辈子。

## 验证

这一章的验证是"全链路"的——从 mini kernel 一路看到 big kernel 的那行字。

构建(注意现在 image 要包含 big kernel):

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -S .
cmake --build build -j$(nproc)
```

host 单测验加载器逻辑:

```bash
cmake --build build --target test_host
```

QEMU 内核测验"真加载"和"压力":

```bash
cmake --build build --target run-kernel-test
```

`test_big_kernel_load` 验真加载流程通(顺带用它附带的 CRC32 断言核对镜像完整性——注意 CRC32 在 Cinux 里是**测试里的独立断言**,不是生产加载路径上的关卡),`test_stress_big_kernel` 拿 1GB 合成 ELF 压 loader。

量产看交棒那一刻:

```bash
cmake --build build --target run
```

串口上先是 mini kernel 一路走完它 005–008 的所有输出,然后加载器的 Phase 1 打印 `[LOADER] Phase 1: Reading ... sectors from LBA ...`、`[LOADER] ELF file: ... bytes (... sectors)`;Phase 2 打印 `Mapping physical memory up to ...`、内存布局表 `[OK] No overlaps detected.`、`[LOADER] Phase 2: Reading ... sectors from disk...`、`[LOADER] Big kernel loaded successfully.`、`[LOADER] Entry point: 0x1000000`。紧接着 mini kernel 跳转,big kernel 的串口出现那行决定性的 **`[BIG] Big kernel running @ 0x1000000`**,然后安静停下。这一行,就是 009 的通过信号,也是整个 mini-kernel 卷的句号。

## 下一站

big kernel 现在能跑了,但它脚下踩的还是 mini kernel 留下的"临时基建"——mini kernel 的 GDT、mini kernel 的临时分页。它自己还没有 IDT,所以全程 `cli`,任何异常一来就三重故障。一个不能被中断、不能扛异常的内核,没法继续往上堆驱动和进程。

下一章 [002 · 大内核的 GDT](../002/),big kernel 要建它自己正式的 GDT(带 TSS、为后面的特权级和中断栈切换准备),把 mini kernel 留下的临时段表换掉。从那以后,big kernel 才算真正"站稳",开始按自己的规矩运行。

---

### 参考

- OSDev — [Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel)(高半加载与跳转架构)、[ELF](https://wiki.osdev.org/ELF)(PT_LOAD、p_offset/p_paddr、加载语义)。
- ELF64 / TIS 规范 — 程序头字段、PT_LOAD 段、`memcpy` 与重叠区(`memmove`)语义。
- 本 tag 源码:[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp)、[boot.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/boot.S)、[kprintf.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/lib/kprintf.cpp)、[big_kernel_loader.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/big_kernel_loader.cpp)(两阶段加载 + 重叠检查)、[elf_loader.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/elf_loader.cpp)(快照 phdr + memmove + 物理入口换算)、[paging.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/arch/x86_64/paging.hpp)(`identity_map_up_to`)、[check_memory_layout.py](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/scripts/check_memory_layout.py)、[generate_large_elf.py](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/scripts/generate_large_elf.py)。
- 调试素材提炼自 [009-01-elf-loader-header-corruption.md](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/document/notes/009/009-01-elf-loader-header-corruption.md)。
