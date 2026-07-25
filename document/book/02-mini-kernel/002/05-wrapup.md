---
title: 05 · 验证与下一站
---

# 验证与下一站

这一章的测试第一次覆盖了"内核数据结构"本身,而不只是 C++ 运行时。

QEMU 内核测试是主战场:

```bash
cmake --build build --target run-kernel-test
```

`test_pmm` 会真刀真枪地验:连续 `alloc_page` 若干次、看 `free_page_count` 是不是按预期递减;把分配到的页 `free` 回去、看计数回升;确认两次分配不会拿到同一页;分配到 OOM 时拿到 `0`。这些断言跑在真内核里(它们依赖位图静态数组、E820 解析,host 测不了)。

量产内核看统计:

```bash
cmake --build build --target run   # 或 make run
```

串口上会看到 `[MINI] PMM: kernel_phys=0x20000, kernel_size=0x... (... pages)`、`marking bootloader 0x0-0x10000 used`、最后一句 `Total N pages (M MB), Free ... pages (... MB)`。那个 Free 数字就是内核此刻能动用的物理内存总量——它得是个合理的正值(比如 QEMU 给 512MB,Free 应该是几百 MB 量级),是 0 或负就是 init 算错了。

## 下一站

内核现在能分配物理页了。可注意:它分到的是**物理地址**,而我们身处 64 位长模式、地址翻译走页表——一个裸的物理地址没法直接当指针用(除非正好在恒等映射的那 8MB 里)。要把"物理页"变成"内核能随便用的虚拟地址",我们需要一层虚拟内存管理(VMM),建内核自己的页表、做物理↔虚拟的映射。

不过在那之前,还有一件更基础的事得先办:内核现在 `cli; hlt` 完全不响应中断,任何异步事件(定时器、键盘)它都接不住。下一章 [003 · 中断](../003/),我们给 mini kernel 装上 IDT 和 PIC,让它第一次能"被打断"并做出响应。中断和内存是内核的两条腿,这一章迈出了内存这条,下一章迈另一条。

---

### 参考

- OSDev — [Physical Memory Management](https://wiki.osdev.org/Physical_Memory_Management)(位图 vs 栈式 vs buddy 分配器取舍)、[Page Frame Allocation](https://wiki.osdev.org/Page_Frame_Allocation)、[Detecting Memory (x86): E820](https://wiki.osdev.org/Detecting_Memory_(x86)#e820)。
- ld 链接脚本 — 符号即地址、`&symbol` 取值的约定(OSDev [Linker Scripts](https://wiki.osdev.org/Linker_Scripts) 与 ld 手册 Symbol 一节)。
- Linux(伙伴系统)/ xv6(`kalloc`/页表)仅作"更高级分配器与虚拟内存"的对比参照,非本 tag 实现。
- 本 tag 源码:[pmm.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/mm/pmm.cpp)/[pmm.h](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/mm/pmm.h)、[mm_defines.h](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/mm/mm_defines.h)、[memory_literals.h](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/mm/memory_literals.h)、[test_pmm.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/test/test_pmm.cpp)、[linker.ld](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/linker.ld)。
- 调试素材提炼自 [006-01-linker-symbol-access.md](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/document/notes/006/006-01-linker-symbol-access.md)、[006-02-object-library-global-ctors-not-called.md](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/document/notes/006/006-02-object-library-global-ctors-not-called.md)。
