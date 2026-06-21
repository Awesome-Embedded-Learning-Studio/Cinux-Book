---
title: 调试档案 006 · PMM 的两个工具链坑
tag: 006_mini_kernel_pmm
---

# 调试档案 006 · PMM 的两个工具链坑

> 从 `notes/006/006-01-linker-symbol-access.md`、`notes/006/006-02-object-library-global-ctors-not-called.md` 提炼,配套 [006 · 物理内存管理](../book/02-mini-kernel/006-mini-kernel-pmm.md)。PMM 本身的算法不难,真正的坑在它依赖的两件工具链约定上:链接器符号怎么取、对象库的全局构造怎么保证被调。

## 案例一:链接器符号取值不带 &,读到的是字节不是大小

- **症状**:PMM 初始化时报告的 `kernel_size` 明显不对——几 KB 的内核报成几百字节,或一个莫名其妙的单字节值(像 `0x55`)。
- **原因**:内核大小来自链接脚本定义的符号 `__kernel_size`。在 C 里声明 `extern char __kernel_size;` 后,正确的取值方式是 `&__kernel_size`——因为链接器符号的"值"就等于它在地址空间里的"地址",取址正好拿到那个数。写成 `__kernel_size`(不带 `&`)则去读了那个地址处的一个字节,得到的是内核镜像某处的字节值,而非大小。
- **定位**:对照量产串口的 `[MINI] PMM: kernel_size=0x...` 和 `mini_kernel.bin` 的实际文件大小;对不上就是符号取值方式错了。GDB 里 `p/x &__kernel_size` 应等于文件大小,`p/x __kernel_size` 等于镜像首字节。
- **修复**:一律用 `&__kernel_size`、`&__mini_kernel_end` 这类取址形式获取链接器符号的值。
- **防复发**:把"链接器符号即地址、取址即取值"刻进约定;获取任何段起止/大小符号(`__bss_start`、`__kernel_size` 等)统一走 `&symbol`。

## 案例二:对象库里的全局对象,构造函数没被调用

- **症状**:把 `pmm.cpp`、`format.cpp` 等编成静态库再链进内核后,某处依赖全局对象构造的代码行为异常(全局对象状态是默认 0,构造函数设的值没生效),但代码看着没错。
- **原因**:全局对象的构造函数指针被编译器放进 `.init_array` 段,启动时由 `_init_global_ctors` 遍历 `__init_array_start..end` 调用。当这些代码在对象库(静态库)里时,如果链接脚本没用 `KEEP` 保护 `.init_array`,或符号导出/链接顺序不对,链接器可能把 `.init_array` 当未引用段裁掉,或起止符号对不齐,导致构造函数表为空、全局构造全跳过。
- **定位**:GDB 看 `__init_array_start` 与 `__init_array_end` 是否相等(相等=段空或被裁);或在构造函数里打个 debugcon 标记,跑起来没标记就是没被调。
- **修复**:链接脚本 `.init_array` 段用 `KEEP(*(.init_array .init_array.*))` 防裁剪,并正确导出 `__init_array_start`/`__init_array_end`;确保含全局对象的翻译单元真正被链入(对象库按需链接,若无人引用其符号可能整个被丢——必要时加 `-Wl,--whole-archive` 或显式引用)。
- **防复发**:任何含全局对象的内核,`.init_array` 一律 KEEP;配一条"全局对象构造"的内核测试(test_cpp_basic 里那条)专门盯它;含全局构造的代码尽量直接编进主目标而非对象库,减少"被链接器优化掉"的风险。

---

### 一句话总结

PMM 这章的两个坑都不在算法、在工具链约定:链接器符号必须 `&` 取值,对象库的全局构造必须用 `KEEP(.init_array)` 守护。这两个约定一旦记牢,后面凡是和"段边界""全局对象"打交道的地方都不会再栽。
