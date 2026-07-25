---
title: 05 · 验证、下一站与参考
---

# 验证、下一站与参考

## 验证

第一道闸还是构建。现在 image 由三段拼成:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -S .
cmake --build build -j$(nproc)
```

`build/kernel/mini/mini_kernel.bin`(以及 mbr.bin/stage2.bin)产出,说明内核这套 freestanding 编译、链接脚本、objcopy 全过了。

第二道闸看 debugcon 序列。`cmake --build build --target run`,看 `build/debug.log`,期望按序出现:

```text
P L J 1 2 3 G 4 ===CPP C1 1 V 2 3 B ===END
```

逐段对应:`P/L`=003 的 PM/长模式;`J`=bootloader 要跳了;`1/2/3`=内核 `_start` 前三步(到、设栈、清 BSS);`G`=全局对象 `global_counter` 的构造(由 `_init_global_ctors` 触发,**夹在 `3` 与 `4` 之间**);`4`=全局构造跑完;`===CPP…===END`=main 的 C++ 冒烟测试;中间的 `1/2/3`=三项测试通过、`B`=BootInfo 校验通过。少了哪一段,就照"调试现场"三个坑对号入座。

第三道闸用 GDB 确认跳进高半。`cmake --build build --target run-debug`:

```text
(gdb) file build/kernel/mini/mini_kernel      # 内核 ELF
(gdb) target remote :1234
(gdb) b *mini_kernel_main
(gdb) c
(gdb) p/x $rdi                # 应是 0x7000(BootInfo*)
(gdb) p/x $rip                # 应在 0xFFFFFFFF8002xxxx 高半
```

断在 `mini_kernel_main`、`rdi=0x7000`、`rip` 在高半,说明交接和跳转都对了。

## 下一站

boot 卷到这里收尾:从 MBR 到长模式、再到第一个 C++ 内核跑起来,整条引导链完整了。bootloader 的活干完了——但它交给内核的,还只是一个"能跑 C++、有一份启动信息"的空壳。内核现在没有内存管理、没有中断、没有进程,甚至连一块能 `new` 的堆都没有(operator new 调到就死)。

接下来是 [02-mini-kernel 卷](../02-mini-kernel/001/):内核从 `mini_kernel_main` 开始真正接管机器——先给自己搭一套物理内存管理(PMM),再处理中断,把自己从一个"会跑 C++ 的空壳"变成一个"能管资源"的小内核。从那以后,主角就是内核自己了。

---

### 参考

- System V AMD64 ABI — 整型参数传递顺序(`%rdi` 为第一参数),BootInfo 交接的依据。
- OSDev — [Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel)(高半内核与临时双映射)、[ELF](https://wiki.osdev.org/ELF)(VMA/LMA、`AT()` 物理落点)、[Detecting Memory (x86): E820](https://wiki.osdev.org/Detecting_Memory_(x86)#e820)、[Calling Global Constructors](https://wiki.osdev.org/Calling_Global_Constructors)(`.init_array` 与裸机 C++ 运行时)、[C++ Bare Bones](https://wiki.osdev.org/C%2B%2B_Bare_Bones)(freestanding 标志、crt 桩)。
- 本 tag 源码:[boot.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/common/boot.S)、[boot_info.h](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/boot_info.h)、[stage2.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/stage2.S)(`long_mode_entry` 填 BootInfo 与跳转)、[long_mode.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/common/long_mode.S)(高半映射)、[linker.ld](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/linker.ld)、[boot.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/arch/x86_64/boot.S)、[crt_stub.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/arch/x86_64/crt_stub.cpp)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/main.cpp)、[build_image.sh](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/scripts/build_image.sh)。
- 调试素材提炼自 [kernel_load_stack_collision.md](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/document/notes/004-B/kernel_load_stack_collision.md)、[boot_info_param_corruption.md](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/document/notes/004-C/boot_info_param_corruption.md)、[bss_data_symbol_conflict.md](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/document/notes/004-C/bss_data_symbol_conflict.md)。

> Intel SDM 版本说明:本卷引用的 SDM 章节号沿用较早版本编号;若按项目本地 PDF(2023-06 版)查阅,内容位置以章节标题为准(System V AMD64 ABI、OSDev 的引用不受此影响)。
