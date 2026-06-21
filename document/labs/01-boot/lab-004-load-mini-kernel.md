---
title: Lab 004 · 加载第一个内核
---

# Lab 004 · 加载第一个内核

> 这个 lab 配套 [004 · 加载第一个内核](../../book/01-boot/004-boot-load-mini-kernel.md),是 boot 卷的收尾。目标:让 bootloader 把一个 C++ mini kernel 从磁盘读进内存、用 BootInfo 交接、跳进高半内核入口,跑通一组 C++ 冒烟测试。**ELF 载入地址、BootInfo 布局、高半映射、crt 桩都得自己搭**,不给现成答案。

## 实验目标

- 在实模式里查 E820 内存图、把内核 ELF 读到物理 `0x20000`。
- 定义两边共用的 `BootInfo`,bootloader 填它、放在 `0x7000`、用 `rdi` 传给内核。
- 在临时页表里加一条高半映射,让内核能在 `0xFFFFFFFF80020000` 跑。
- 写内核 `boot.S`(清 BSS、把 boot_info 存进 `.data`、跑全局构造、调 main)和 `crt_stub.cpp`,让裸机 C++ 能起来。
- 验证:debugcon 出现 `PLJ1234G===CPP…B…===END`。

## 前置条件

- 已完成 Lab 003:机器能进 64 位长模式,`debug.log` 有 `L`。
- 会写 freestanding C++、看得懂链接脚本的 VMA/LMA、知道 `.init_array` 是什么。

## 任务分解

分五块走,别想一口吃下。

### 第一块:实模式读内核 + 内存图

写 `boot/common/boot.S`:`query_memory_map`(E820 `INT 0x15 AX=0xE820`,条目存 `0x5000`)、`load_kernel_from_disk`(从 LBA 16 读 832 扇区到 `0x20000`)。在 stage2 实模式段调它们。注意读盘地址要和内核链接脚本的 LMA 对上。

### 第二块:BootInfo 交接

写 `boot/boot_info.h` 定义 `BootInfo`(显式定长类型 + `packed`,`static_assert(sizeof==824)`)。在 `long_mode_entry` 里把帧缓冲(从 `0x6400`)、内存图(从 `0x5000`)抄进 `0x7000` 的 `BootInfo`,填好 entry/phys_base。最后 `movq $0x7000,%rdi; jmp *0xFFFFFFFF80020000`。

### 第三块:高半映射

在 `setup_page_tables` 里,除了低地址恒等映射,再加 `PML4[511]→PDPT`、`PDPT[510]→PD`,复用同一张 PD,让高半虚拟地址指向和低地址同一块物理页。不加这条,跳进高半就缺页三重故障。

### 第四块:内核 boot.S

写 `kernel/mini/arch/x86_64/boot.S` 的 `_start`:`cli` → 设栈(`__mini_stack_top`)→ **把 `%rdi` 存进 `.data` 的 `__boot_info_ptr`** → 清 BSS → `call _init_global_ctors` → `call mini_kernel_main(BootInfo*)`。每步用 debugcon 打个标记(`1234`)方便定位。

### 第五块:crt_stub + main 冒烟测试

写 `crt_stub.cpp`:`_init_global_ctors`(遍历 `.init_array`)、`__cxa_pure_virtual`、`operator new/delete`(调到就 `hlt`)等桩。写 `main.cpp` 的 `mini_kernel_main`:测一个普通类、一对虚函数类、一个全局对象,再校验 `BootInfo`,输出 `===CPP…===END`。

## 接口约束

这些得自己保证对、lab 不给现成代码:

- **载入地址**:内核 ELF 读到物理 `0x20000`,LBA 16 起。和 `linker.ld` 的 `AT(0x20000)` 对上(别信 stage2 注释里的 0x88000 噪声)。
- **BootInfo**:824 字节,放物理 `0x7000`;bootloader 填、内核经 `rdi` 读。字段用 `uint32_t/uint64_t` + `packed`,32 位 boot 和 64 位 kernel 布局必须一致。
- **传参**:第一参数 `BootInfo*` 走 `%rdi`(System V AMD64 ABI)。bootloader `movq $0x7000,%rdi` 后 `jmp`。
- **高半**:VMA `0xFFFFFFFF80020000`,LMA `0x20000`;页表里 PML4[511]/PDPT[510] 复用 PD。
- **`__boot_info_ptr` 必须在 `.data`**:`.bss` 会被清零,存进去就丢。
- **顺序**:`_start` 里"存 boot_info → 清 BSS → 全局构造 → main",顺序不能乱。
- **编译标志**:`-ffreestanding -fno-exceptions -fno-rtti -fno-pie -mcmodel=large -mno-red-zone -nostdlib`。
- **盘布局**:扇区0=MBR、1..15=stage2、16+=kernel;`build_image.sh` 写三段。

## 验证步骤

**第一道:构建。**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -S .
cmake --build build -j$(nproc)
```

**第二道:debugcon 序列。** `cmake --build build --target run`,`cat build/debug.log`,期望:

```text
P L J 1 2 3 4 G ===CPP C1 1 V 2 3 B ===END
```

`B` 出现 = BootInfo 交接成功;`1/2/3` = C++ 运行时(类/虚函数/全局构造)正常。

**第三道:GDB。** `cmake --build build --target run-debug`,断在 `mini_kernel_main`,看 `rdi=0x7000`、`rip` 在高半。

## 常见故障

**内核加载就崩 / 一进 PM 就崩。** 内核和栈(`0x90000`)撞了。把载入地址定低(`0x20000`),和栈之间留 gap。低 1MB 是兵家必争地,画清地址边界。

**内核跳进去了、main 也跑了,BootInfo 字段全是 0(`B` 不出现)。** `boot_info` 存进了 `.bss`,被清零抹掉。改存 `.data` 的 `__boot_info_ptr`,且"存"在"清 BSS"之前。

**加了虚函数/全局对象后链接报 `undefined reference`。** `-nostdlib` 砍了 C++ 运行时。补 `crt_stub.cpp`:`__cxa_pure_virtual`、`operator delete`、`_init_global_ctors` 等;链接脚本导出 `__init_array_start/end`。

**跳进高半入口就三重故障。** 高半没映射。`setup_page_tables` 加 PML4[511]/PDPT[510] 复用 PD。

**全局对象的构造没跑(冒烟测试的 `G`/`3` 不出现)。** 没调 `_init_global_ctors`,或 `.init_array` 符号没导出对。确认 `boot.S` 在 main 前 `call _init_global_ctors`,链接脚本里 `__init_array_start/end` 正确包裹 `.init_array` 段。

## 通过标准

- `cmake --build build` 成功,`mini_kernel.bin` 等三段产出,`cinux.img` 拼好。
- `make run` 后 `debug.log` 出现完整序列,含 `B`(BootInfo 校验通过)和 `1/2/3`(C++ 冒烟测试通过)。
- GDB 能断在 `mini_kernel_main`,`rdi=0x7000`、`rip` 在高半。
- 全程没有 PMM/堆/中断——内核还只是个"会跑 C++ 的空壳",那些是 [02-mini-kernel 卷](../../book/02-mini-kernel/005-mini-kernel-entry.md) 的 lab。
