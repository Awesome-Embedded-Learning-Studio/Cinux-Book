---
title: 调试档案 004 · 第一次加载内核的三个连环坑
tag: 004_boot_load_mini_kernel_C
---

# 调试档案 004 · 第一次加载内核的三个连环坑

> 从 `notes/004-A`、`004-B`、`004-C` 提炼,配套 [004 · 加载第一个内核](../book/01-boot/004-boot-load-mini-kernel.md)。004 的 A/B/C 三个 tag 不是三个功能,而是"让第一个 C++ 内核跑起来"时连续踩中的三个坑。它们在源码注释里都留下了修复痕迹,适合做成可查的案例。

## 案例一:内核加载和栈撞在一起(004_A→B)

- **症状**:内核刚加载、或一进保护模式/长模式就崩,debugcon 序列停在很早的位置。
- **原因**:实模式下能用的只有低 1MB 内存,而 MBR、Stage2、保护模式栈(`0x90000`)、内核加载区全挤在这片。早期版本把内核读到了和 `0x90000` 栈重叠的区域,几层函数 `call`/`push` 压栈,就把刚加载的内核代码盖掉了。
- **定位**:看 `load_kernel_from_disk` 的目标地址和 `pm_entry` 设的 `esp`(`0x90000`)是否重叠;GDB 在读盘后、进 PM 前看内核镜像区域有没有被改。
- **修复**:把内核载入地址定在更低的 `0x20000`,与栈 `0x90000` 之间留出明确 gap(stage2 注释 "leaving 32KB gap before protected mode stack at 0x90000" 即此修复的备忘)。
- **防复发**:低 1MB 是"兵家必争之地",维护一张地址分配表(MBR/栈/BIOS 数据区/内核加载区/VESA 块/BootInfo/E820),任何新增占用先查表别撞车。

## 案例二:BootInfo 传过去就损坏(004_B→C)

- **症状**:内核跳进去了、`mini_kernel_main` 也跑了,但读 `BootInfo` 字段全是 0 或乱码,BootInfo 校验标记(`B`)印不出来。
- **原因**:早期版本把 bootloader 传来的 `BootInfo*`(`%rdi`)存进了一个 `.bss` 段的变量。而 `boot.S` 的 `_start` 紧接着会用 `rep stosb` 清零整个 `.bss`——刚存的指针被抹成 0,后面 main 拿到的 `BootInfo*` 就是 0,解引用读到全 0。
- **定位**:GDB 在 `_start` 的"存指针"和"清 BSS"两步前后,分别看那个变量的值;清 BSS 后变 0 就是中招。
- **修复**:把 `__boot_info_ptr` 放进 **`.data` 段**(已初始化数据,不在清零范围);并且**"存指针"必须在"清 BSS"之前**。源码 `/* Save BootInfo pointer BEFORE clearing BSS */` 就是这条教训。
- **防复发**:`.bss` 的语义是"启动时为 0",任何"需要在清零后存活"的值都不能放 `.bss`;启动期保存的指针、句柄一律放 `.data`。

## 案例三:裸机 C++ 的符号缺失 / 全局构造没跑(004_C)

- **症状**:给内核加上带虚函数的类、全局对象后,要么链接器报 `undefined reference to __cxa_pure_virtual / operator delete / ...`,要么链接过了但全局对象构造没执行(冒烟测试的 `G`/`3` 不出现)。
- **原因**:`-nostdlib -ffreestanding` 砍掉了 C/C++ 运行时。但凡用到虚函数(需 `__cxa_pure_virtual`)、虚析构(需 `operator delete`)、全局对象(需遍历 `.init_array` 跑构造),就会缺符号或缺"跑构造的那段代码"。
- **定位**:看链接错误缺哪些符号;链接过了就看 `_start` 有没有在 `main` 之前调用全局构造遍历。
- **修复**:写 `crt_stub.cpp` 补齐——`_init_global_ctors`(遍历 `__init_array_start..end`)、`__cxa_pure_virtual`、`__stack_chk_fail`、`__cxa_atexit`、`operator new/delete`(`hlt` 桩);链接脚本正确导出 `__init_array_start/end`、`__bss_start/end`。
- **防复发**:裸机 C++ ≠ "去掉 main 的普通 C++"。把"清 BSS + 跑全局构造 + 运行时符号桩"当成内核启动的固定三件套,缺一不可。

---

### 一句话总结

第一次加载内核的三个坑,根子都在"边界":**内核加载区与栈的地址边界**、**boot_info 存放位置与 BSS 清零的时序边界**、**裸机 C++ 与语言运行时的供给边界**。把这三条边界在启动流程里画清楚、钉死,内核就能稳稳地从 bootloader 手里接过接力棒。
