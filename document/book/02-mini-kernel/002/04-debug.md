---
title: 04 · 调试现场
---

# 调试现场

这一章的坑,一个是"链接器符号怎么取",一个是"哪些内存不能分"。

第一个就是上面讲的 `__kernel_size`。症状是 PMM 报告的内核大小明显不对(比如几 KB 内核报成几百字节,或一个莫名其妙的单字节值)。根因是写成 `__kernel_size`(不带 `&`),读到了那个地址处的字节而非符号值。修复就是 `&__kernel_size`。判断方法:量产内核串口上的 `[MINI] PMM: kernel_size=0x...` 那行,大小应该和 `mini_kernel.bin` 的文件大小一致——对不上就是符号取值错了。

第二个是忘了滤低 1MB。症状是 alloc 出来的页落在 `0x0–0x100000` 之间,一用就踩到 bootloader 或 BIOS 数据区,内核莫名其妙崩。根因是 init 没把低 1MB 标占用(或 E820 报告了低区可用但没滤)。修复就是那个 `LOW_MEMORY_BOUNDARY` 截断逻辑。判断方法:连续 alloc 几页,看返回地址是不是都 ≥ 1MB。

第三个(也是 001 末尾预告、002 notes 里专门记的)是对象库与全局构造。`pmm.cpp`、`format.cpp` 这些被编成静态库再链进内核,如果链接/构造链没接对,全局对象的构造不被调用。这一章的测试头 [kernel_test.h](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/test/kernel_test.h) 抽成公共头、`linker.ld` 用 `KEEP(*(.init_array))`,都是为了堵这条线。判断方法:`test_cpp_basic` 里"全局对象构造"那条过没过。
