---
title: 04 · 调试现场、验证与下一站
---

# 调试现场、验证与下一站

## 调试现场

这一章没有 notes 文件,但有两个真实的、装配时高发的坑,值得点出来。

**第一个,「屏幕是黑的但串口有输出」。** 这是最常见的症状,根因几乎总是上面说的装配顺序里某一环断了。排查思路是沿着依赖链倒着查:串口有输出,说明 `kprintf_init` 成功了、串口 sink 在工作;屏幕黑,说明 console sink 没生效。那就查——`console.init` 调了吗?它之前 `fb.init` 和 `font.init` 调了吗?`kprintf_register_sink(console)` 调了吗?最阴的情况是:`register_sink` 调了,但 console 的 `fb_` 是 nullptr(因为 fb 还没 init),于是 `putc` 里第一行 `if (fb_ == nullptr) return;` 直接返回,字符被静默吞掉。`putc` 开头那个 nullptr 检查是个保护,但也意味着「配置错了不会崩,只会安静地不显示」——遇到屏幕黑,别只盯着 console 代码看,先确认它依赖的 fb、font 真的初始化了。

**第二个,「同一个字符在串口和屏幕上不一致」或者「屏幕上少了字符」。** 既然是 fan-out,正常情况下两路应该一字不差。如果屏幕少了字符,多半是 console 的状态机在某个控制符上和串口行为不一致——比如串口对某个字节照单全收,而 console 的 `putc` 把它当成了需要特殊处理的字符,或者 `col_ >= cols_` 的自动换行比预期早触发,把一个字符挤掉了。这种问题用对照法最直接:让内核打一段包含换行、长行、特殊字符的固定文本,串口和屏幕逐字比,第一个不一致的地方就是 bug 现场。

还有一个值得意识到的点:fan-out 意味着**每个字符都被处理两遍**(串口一遍、屏幕一遍)。屏幕那路还要走字体渲染、逐像素 `put_pixel`,比串口慢得多。所以一段很长的 kprintf 输出,瓶颈永远在屏幕渲染这一路。此刻这不构成问题(诊断输出量不大),但心里有这个数,以后如果发现内核启动变慢、又恰好在打一大段日志,就知道去哪找。

## 验证

这一章的验证,核心就是「双输出真的生效了」。

最直接的现象验证:把内核 `make run`(或对应 CMake target)起来,你会看到 012 就有的那段 kprintf 格式回归输出,**现在不仅在串口、也在屏幕上**刷出来。关键看那句:

```text
[BIG] Console initialised -- dual output active.
```

它出现在屏幕上,就证明 console sink 注册成功、fan-out 在工作。从这句之后,所有 `[BIG]` 开头的诊断都是串口和屏幕同步的。

机内测里,[test_video.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_video.cpp) 也覆盖了 console,验证它能正确地 `putc`、能在写满后触发滚动。和 013 的 fb/font 测试一起跑:

```bash
cmake --build build --target run-big-kernel-test
```

console 的纯逻辑(光标移动、换行、回卷、控制符处理),有 host 单测 [test_console.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_console.cpp) 镜像测,不依赖 QEMU:

```bash
ctest --test-dir build -R console --output-on-failure
```

这些测的是 `putc` 状态机的算术:写 N 个字符后 `(row, col)` 在哪、`\n` 之后在哪、写满一行触发换行没触发滚动、写满整屏才触发滚动。它们绿的,console 的行为就是对的。

## 下一站

到这里,内核第一次有了「脸」:它能在屏幕上画字,kprintf 的每一句话都能直接在屏幕上看见,不用再开串口窗口。诊断通道从一根线(串口)变成了两根线(串口 + 屏幕),双输出稳定工作。

但你会发现一个明显的缺口:这台机器**只会说,不会听**。屏幕能显示,可键盘敲进去的字符,内核一个都收不到——IRQ1 上挂的还是那个只发 EOI 就把字符丢掉的 default handler,和 011 结束时一模一样。我们装了一整套中断体系,却还听不见键盘。

打破这个局限,就是下一站的事。键盘要接上真 handler,得搞定扫描码、IRQ1 的真处理、还有怎么把收到的字符送回屏幕(你会发现,刚搭好的 console 又要派上用场了)。不过那是 [下一章](../008/) 的故事,这里我们先享受一下「内核终于能在屏幕上说话」这个里程碑。

---

### 参考

- 012 章 · [kprintf 重构与引导期 SSE 初始化](../005/):`vkprintf_impl` 回调式格式化引擎的来历。本章的多路 sink 正是建立在那层回调解耦之上——引擎未改,只换了输出分派。
- OSDev — [Text UI / Text Mode Console](https://wiki.osdev.org/Text_UI):在帧缓冲上手搓文本控制台时,光标跟踪、自动换行、滚动这几件事的常见做法。本章 Console 的状态机与此一致(只是画在图形帧缓冲上,而非 VGA 文本模式)。
- 本 tag 源码:[console.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/video/console.hpp) / [console.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/video/console.cpp)、[kprintf.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/lib/kprintf.hpp) / [kprintf.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/lib/kprintf.cpp)(多路 sink)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp)(装配序);驱动重组见 [pit](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/pit/)、[serial](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/serial/);测试 [test_console.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_console.cpp)、[test_video.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_video.cpp)。
