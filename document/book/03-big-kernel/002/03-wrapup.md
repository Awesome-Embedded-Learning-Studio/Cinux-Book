---
title: 03 · 验证与下一站
---

# 验证与下一站

## 验证

讲完了,得能跑出来。`make run-big-kernel-test` 会在 QEMU 里跑一组测试,其中四条直接读段寄存器:

```cpp
void test_cs_register() {
    uint16_t cs = 0;
    __asm__ volatile("movw %%cs, %0" : "=r"(cs));
    TEST_ASSERT_EQ(cs, GDT_KERNEL_CODE);   // 期望 0x08
}
```

`DS/SS/ES` 同理期望 `0x10`。要是 `lgdt` 之后忘了刷新 `CS`、或选择子算错位,这几条断言当场挂——比在真机上莫名其妙重启友好太多了。

手动看的话,`make run` 会打印:

```text
[BIG] Big kernel running @ 0x1000000
[BIG] GDT loaded.
```

看到 `GDT loaded.` 就说明 `init()` + `load()` 一路走通。

## 下一站

到这里,big kernel 脚下有了正经的段地基,`TR` 也挂上了 TSS。可你要是现在故意触发一个异常(比如 `int $3`),内核会直接三重故障重启——因为我们**还没有 IDT,没有任何异常兜底**。

这正是 [003 · IDT 与异常](../003/) 要干的活:建 IDT、写 ISR、让 `int $3` 被接住、dump 出寄存器、然后活着继续跑。GDT 是地基,IDT 是安全网,两篇合起来,内核才第一次"扛得住事"。

---

### 参考

- Intel SDM Vol.3A — §3.4.5 Segment Descriptors(段描述符格式与位定义)、Long Mode 下的段角色、§8.7 + Figure 8-11(64 位 TSS 布局)。注:源码注释写的 "Table 8-2" 实为 32 位 TSS 的 Figure 8-2,64 位应为 Figure 8-11。
- OSDev — [Global Descriptor Table](https://wiki.osdev.org/Global_Descriptor_Table)、[Task State Segment](https://wiki.osdev.org/Task_State_Segment)。URL 有效性同样待核实。
- 本 tag 源码:[gdt.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/gdt.hpp)、[gdt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/gdt.cpp);测试 [test_gdt_idt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_gdt_idt.cpp)、[test_gdt_idt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_gdt_idt.cpp)。
