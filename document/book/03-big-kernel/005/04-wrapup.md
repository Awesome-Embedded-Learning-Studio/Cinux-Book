---
title: 04 · 验证与下一站
---

# 验证与下一站

## 验证

kprintf 的格式化逻辑,直接跑那批 host 单测,不依赖 QEMU:

```bash
ctest --test-dir build -R kprintf --output-on-failure
```

四十来个用例覆盖了所有 specifier、宽度对齐、`nullptr`、负数零补、混合格式和未知 specifier 兜底。它们绿的,格式化引擎就是对的。

SSE 的修复,验证方式反过来——得用 `-O2` 把内核编出来跑,确认它不再 Triple Fault:

```bash
cmake -B build-release -DCMAKE_BUILD_TYPE=Release <其余参数>
cmake --build build-release --target run-kernel-test
```

修复前这会在 `idt_init` 阶段 `QEMU unexpected exit code: 0`;修复后能稳稳跑完全部 22 项内核测试并 `exit 0`(经 isa-debug-exit 的正常退出)。顺带,`run` 起来后你会先看到一段 kprintf 的格式回归输出——那是 main 里新加的那 19 行,把 `%08x`、`%-10s`、`%p` 这些实打实打了一遍:

```text
[KPRINTF] %08x: 0000dead
[KPRINTF] %-10d: 42        |
[KPRINTF] %p: 0x00001234ABCD5678
[KPRINTF] mix: test n=99 hex=cafebabe ptr=0x0000000000000001
```

## 下一站

地基夯实了,该往上盖了。到现在为止,内核所有的输出还只走串口一条路——你得开着 QEMU 的串口窗口才看得见它在说什么。下一个 tag 013 终于要把 framebuffer 接上,让内核能直接在屏幕上画字: framebuffer 驱动、字体、console。到那时,这一章给 kprintf 抽出来的那个回调式引擎,会迎来它的第二个输出后端——屏幕。我们早就为这一天留好了接口。

顺带,013 还会把 drivers 目录理一理(serial、pit 各自挪进自己的子目录),那个一直被 tag 名挂着、却没在这一章出现的「serial driver 化」,到那时才算真正落地。

---

### 参考

- Intel SDM Vol.3(System Programming,控制寄存器):`CR4.OSFXSR`(bit 9)、`CR4.OSXMMEXCPT`(bit 10)、`CR0.TS/EM/MP`。本地 PDF `document/reference/intel/SDM-Vol3A-*.pdf`,可用 `pdf-reader` 搜索 "OSFXSR"/"OSXMMEXCPT" 复核位号。
- Intel SDM Vol.2(指令参考):SSE2 指令(`PXOR`/`MOVAPS`)在 `CR4.OSFXSR = 0` 时触发 `#UD` 的规则;`CLTS`、`FXSAVE`/`FXRSTOR` 语义。
- OSDev — QEMU exit devices / isa-debug-exit:退出码 `(value<<1)|1` 恒为奇数,退出码 0 在 `-no-reboot` 下表示 Triple Fault。
- 本 tag 源码:[vkprintf_impl.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/lib/private/vkprintf_impl.hpp)、[kprintf.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/lib/kprintf.cpp)、[kprintf.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/lib/kprintf.hpp)、[boot.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/arch/x86_64/boot.S)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp);测试 [test_kprintf.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_kprintf.cpp);排查笔记 [012-01-sse-init-crash-o2.md](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/document/notes/012/012-01-sse-init-crash-o2.md)。
