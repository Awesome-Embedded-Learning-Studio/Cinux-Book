---
title: Lab 009 · 大内核登场
---

# Lab 009 · 大内核登场

> 这个 lab 配套 [009 · 大内核登场](../../book/03-big-kernel/009-large-kernel-entry.md)。目标:让 big kernel 跑起来,串口出 `[BIG] Big kernel running @ 0x1000000`;mini kernel 用升级版 loader(两阶段 + 重叠检查)把它从磁盘读进来、跳进物理入口。**boot.S/loader 流水线/重叠检查都自己搭**,不给现成答案。

## 实验目标

- 建 big kernel 新树 `kernel/`:boot.S(cli/栈/BSS/ctors/kernel_main)、crt_stub、io、serial、kprintf、linker、main(打印 `[BIG] ... @ 0x1000000`)。
- mini kernel 的 loader 升级:`load_big_kernel` 拆成两阶段(phase1 读头探大小、phase2 扩映射+重叠检查+读完整+load_elf),返回物理入口。
- mini main 调 `load_big_kernel` 并 `jmp` 到返回的**物理入口**(load_elf 已把高半 e_entry 换算成物理)。
- 写运行时重叠检查 + 构建期 `check_memory_layout.py`;配压力测试。

## 前置条件

- 完成 [Lab 008](../02-mini-kernel/lab-008-load-large-kernel.md):mini kernel 有 ATA/ELF loader、能 demo 读盘解析。
- 理解 ELF PT_LOAD、identity 映射、System V AMD64 ABI(`%rdi` 传参)。

## 任务分解

分五块走。

第一块,big kernel 新树。在 `kernel/` 下写 `main.cpp`(`kernel_main`→`kprintf_init`→打 `[BIG]`→halt)、`arch/x86_64/boot.S`(cli→`__kernel_stack_top`→清 BSS→`_init_global_ctors`→`call kernel_main`,BootInfo* 暂传 NULL)、crt_stub、io.hpp、`drivers/serial`、`lib/kprintf`(带 `kprintf_init`)、`linker.ld`(高半 VMA `0xFFFFFFFF80000000`+LMA `0x1000000`)。CMake 把它编成标准 ELF。

第二块,loader 两阶段。`load_big_kernel_phase1`:只读 ELF 头几个扇区→验 magic→把 program header 快照到局部数组→由 `p_offset+p_filesz` 算整份 ELF 大小。`load_big_kernel_phase2`:按大小 `identity_map_up_to` 扩恒等映射→登记页表/mini kernel/PT_LOAD 目标区做两两重叠检查(staging 故意不登记,因为 load-in-place 允许它和 PT_LOAD 同址)→读完整 ELF→`load_elf`。`load_big_kernel` 串起两阶段。

第三块,跳过去。`load_elf` 返回入口(若 e_entry 落高半则减 `0xFFFFFFFF80000000` 换算成物理 `0x1000000`)。mini main 拿到这个**物理**入口后 `jmp` 过去(靠恒等映射落地)。

第四块,kernel_main。big kernel 的 `kernel_main`:`kprintf_init`(初始化它**自己**的串口)→打 `[BIG] Big kernel running @ 0x1000000`→halt。

第五块,安全与压力。loader 里实现 `check_memory_overlaps`(运行时,加载前查致命重叠);写 `scripts/check_memory_layout.py`(构建期辅助)、`scripts/generate_large_elf.py`(生成 1GB 合成 ELF 压 loader)。配 `test_big_kernel_load`、`test_stress_big_kernel`、host `test_big_kernel_loader.cpp`。

## 接口约束

这些得自己保证对、lab 不给现成代码:

- 地址:big kernel 物理 LMA `0x1000000`(16MB)、staging `0x1000000`(load-in-place,与 PT_LOAD 目标同址,允许)、盘上 LBA 848 起;ELF 头里 e_entry 是高半 `0xFFFFFFFF81000000`,但 load_elf 返回/mini 跳转的是**物理 `0x1000000`**。
- big kernel boot.S:第一条必须 `cli`(还没有自己的 IDT);`kernel_main` 的 BootInfo* 暂为 NULL(清 BSS 冲了 %rdi)。
- big kernel 有**自己独立的** kprintf/serial(不复用 mini 的),且需先 `kprintf_init`。
- 加载用 `memmove`(不用 `memcpy`)拷段,且动数据前先把 program header 快照到本地——因为 staging 与 p_paddr 重叠,`memcpy` 会边读边覆盖 ELF 头。
- 重叠检查:登记页表(`0x1000–0x4000`)、mini kernel(`0x20000` 起)、各 PT_LOAD 目标;**不**登记 staging(load-in-place 故意重叠)。发现 mini kernel/页表被打就中止加载。
- **注意:CRC32 在 Cinux 里是 `test_big_kernel_load` 里的独立测试断言,不是生产加载路径上的关卡**——别往 loader 里塞 CRC 拒载逻辑。
- big kernel 这章极简:只 `[BIG] ... running` + halt,不碰 GDT/IDT/驱动/进程。

## 验证步骤

构建(image 现含 big kernel):

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -S .
cmake --build build -j$(nproc)
```

host 单测(loader 编排):

```bash
cmake --build build --target test_host
```

QEMU 内核测试(真加载 + 压力):

```bash
cmake --build build --target run-kernel-test
```

`test_big_kernel_load` 验真加载(含 CRC32 镜像完整性断言),`test_stress_big_kernel` 拿 1GB 合成 ELF 压 loader。

量产看交棒:

```bash
cmake --build build --target run
```

串口:mini kernel 走完它的输出 → `[LOADER] Phase 1: Reading ... from LBA ...`、`ELF file: ... bytes` → Phase 2 `Mapping physical memory up to ...`、`[OK] No overlaps detected.`、`Phase 2: Reading ... sectors`、`Big kernel loaded successfully.`、`Entry point: 0x1000000` → 跳转 → **`[BIG] Big kernel running @ 0x1000000`**。

## 常见故障

`[LOADER]` 之后 QEMU 直接退出、没有 `[BIG]`。加载或跳转出问题。看 Phase 1/2 报错、`load_elf` 返回的入口是不是物理 `0x1000000`(不是高半)。

跳进去三重故障。入口算错——mini 跳的应是 load_elf 返回的**物理**入口(它已把高半 e_entry 减基址);或恒等映射没扩到落点。`identity_map_up_to` 覆盖到 big kernel 物理落点。

big kernel 进去打不出字 / 乱码。忘了 `kprintf_init`,或误用 mini 的 kprintf。big 有自己的串口,进来先初始化。

`test_big_kernel_load` 打完 PT_LOAD[0] 就崩、没 "Loaded segment"。ELF 头自毁:staging 与 p_paddr 重叠,用了 `memcpy` 边读边覆盖头。改用 `memmove` + 动数据前快照 program header 到本地数组。

重叠检查漏报、加载时覆盖了 mini kernel 自己。`check_memory_overlaps` 漏登记了 mini kernel 区或页表区,或把 staging 也登记了(它本就和 PT_LOAD 重叠,登记会误报)。核对登记的区域集合。

误以为要给 loader 加 CRC32 拒载。CRC32 在 Cinux 里只是 `test_big_kernel_load` 的独立断言,生产 loader 不做 CRC——加载期的安全靠重叠检查,不是 CRC。

## 通过标准

- `test_host` 里 loader 单测过;`test_big_kernel_load`(含 CRC32 断言)通过。
- `test_stress_big_kernel`(1GB 合成 ELF)不崩。
- 量产 `make run` 串口出现 `[BIG] Big kernel running @ 0x1000000`。
- big kernel 全程 `cli`、无自己的 GDT/IDT(那是 [010](../../book/03-big-kernel/010-big-kernel-gdt.md));BootInfo 暂未用。
