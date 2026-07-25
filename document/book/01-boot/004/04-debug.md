---
title: 04 · 调试现场:内核撞栈、BootInfo 损坏、裸机 C++ 符号
---

# 调试现场:内核撞栈、BootInfo 损坏、裸机 C++ 符号

004 的 A/B/C 三个 tag,本质上就是"让第一个内核跑起来"过程中踩的三个连环坑。这三个坑在源码注释里都留下了修复痕迹,是非常好的教材。

**坑一(004_A→B)**——内核加载和栈撞在一起。症状:内核刚加载、或一进保护模式就崩。根因:内核被读到了和"保护模式栈(0x90000)"重叠的区域,几层函数压栈就把内核代码盖掉了。修复:把内核载入地址定在更低的 `0x20000`,和栈 `0x90000` 之间留出足够 gap(stage2 那句注释 "leaving 32KB gap before protected mode stack at 0x90000" 就是这次修复的备忘)。教训:**低地址那片 1MB 是"兵家必争之地"——MBR、栈、BIOS 数据区、内核加载区全挤在这**,地址分配必须画清边界,谁也别踩谁。

**坑二(004_B→C)**——BootInfo 传过去就坏了。症状:内核跳进去了、main 也跑了,可一读 `BootInfo` 字段全是 0 或乱码(`B` 标记印不出来)。根因:早期版本把 `BootInfo*`(`rdi`)存进了一个 `.bss` 变量;而 `boot.S` 紧接着会清零整个 `.bss`——刚存的指针被抹成 0。修复:把 `__boot_info_ptr` 放到 **`.data` 段**(已初始化数据,不在清零范围内),并且"存指针"必须在"清 BSS 之前"。源码里那句 `/* Save BootInfo pointer BEFORE clearing BSS */` 就是这条血的教训。

**坑三(004_C)**——裸机 C++ 的符号冲突 / 链接失败。症状:加上带虚函数的类、全局对象后,链接器报 `undefined reference to __cxa_pure_virtual / operator delete / ...` 一堆错,或全局对象的构造没跑。根因:`-nostdlib` 砍掉了 C++ 运行时,但凡用到虚函数(需要 `__cxa_pure_virtual`)、虚析构(需要 `operator delete`)、全局对象(需要 `.init_array` 遍历)就会缺符号或行为不对。修复:写 `crt_stub.cpp` 补齐这些桩 + `_init_global_ctors`,并在链接脚本里正确导出 `__init_array_start/end`、`__bss_start/end`。教训:**裸机 C++ 不是"去掉 main 的普通 C++"**,你得自己把语言运行时那一层补回来。
