---
title: 04 · 调试现场、验证与下一站
---

# 调试现场、验证与下一站

## 调试现场

这一章没有 notes 文件,但键盘驱动有四个高频坑,几乎每个第一次写 PS/2 驱动的人都会撞上至少一个。

**第一个,也是最经典的:「按一下就哑了」。** 你按下第一个键,屏幕回显出来了;再按第二个,没反应;之后怎么按都没反应。十有八九是 handler 里漏了 `PIC::send_eoi(1)`。EOI(End-Of-Interrupt)是告诉 PIC「这个中断我处理完了,你可以再发下一个了」。不发 EOI,PIC 就认为这个 IRQ 还没处理完,再也不给你送同优先级及以下的中断——键盘(IRQ1)就此哑掉。注意 011 时就立过规矩:EOI 由 C handler 自己发,不是汇编 stub 发。所以 `irq1_handler` 的最后一行 `PIC::send_eoi(1)` 是命脉,漏了或写错了 IRQ 号(写成 `send_eoi(0)`),键盘就只响一下。遇到「按一下就没反应」,第一件事就是查 EOI。

**第二个:「字符全是乱的」。** 你按 `a` 出来的是别的字母,或者一堆乱码。这通常是 config 的 **bit6(set2→set1 翻译)没开**。前面说过,键盘默认发 set 2,我们的查找表是 set 1 的。bit6 没置位,你从 `0x60` 读到的就是 set 2 的码,拿它去查 set 1 的表,当然全错。验法是:在 `init` 改完 config 后,把那个 `config` 字节用 kprintf 打出来,确认 bit6(0x40)被置上了。值得一提的是,QEMU 的 PS/2 模拟在某些配置下默认行为和真机略有差异,所以这个 bit 在 QEMU 上可能「不开也碰巧能工作」,但搬到真机就乱——养成「显式置位、不依赖默认」的习惯,能少踩这种「QEMU 上好好的、真机上崩」的坑。

**第三个:扩展键(方向键、右 Ctrl/Alt 等)「没反应」。** 这些键的扫描码带 `0xE0` 前缀(比如方向键是 `0xE0 0x48`)。这一版的 handler 见到 `0xE0` 直接 `send_eoi` 丢弃了,根本没处理前缀后的那一字节。所以方向键、Home/End、右侧的 Ctrl/Alt 这一章都不可用——这是个**已知的有意限制**,不是 bug。把它说清楚:要支持扩展键,handler 得维护一个「上一个是 0xE0」的状态,把后续字节路由到另一张扩展键表。这一章为了把主线(普通键 + 修饰键)讲透,先不碰它。

**第四个:PS/2 时序导致「init 偶尔失败」。** 表现是 `init` 里的 self-test 偶尔读不到 `0x55`,或者配置写进去没生效。根因是命令发太快——控制器还没消化完上一条,下一条就来了。对策就是前面 `send_command` 里那个 `wait_input_empty`:每条命令前等 `INPUT_FULL` 清掉。如果碰到间歇性初始化失败,先检查每条控制器命令前有没有老老实实等输入缓冲空。另外,真机上某些老 8259/PIC 组合对连续 I/O 写有 timing 要求,可能还需要 `io_wait`(往 port `0x80` 写一字节制造约 1µs 延时)——这一章在 QEMU 上用不到,但搬真机时要心里有数。

## 验证

键盘驱动的验证,难点在于「按键」是个外部物理动作,不好自动化。Cinux 的解法是分两层:纯逻辑在 host 上用单测镜像测,真硬件交互在 QEMU 里用「注入扫描码」测。

纯逻辑(扫描码解码、修饰键状态机、ring buffer 算术)完全不依赖真键盘,所以 [test_keyboard.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_keyboard.cpp) 把这些逻辑镜像了一份,在 host 上 `-O2` 编、用 `CINUX_HOST_TEST` 门控,跑了四十多个 `TEST` 用例、近百条断言:

```cpp
TEST("keyboard: scancode 0x1E -> 'a' (lowercase)") { ... }
TEST("keyboard: scancode 0x1E + shift -> 'A' (uppercase)") { ... }
TEST("keyboard: break code 0x9E has make_code 0x1E") { ... }
TEST("keyboard: LShift press sets shift modifier") { ... }
// ... 还有 ring buffer 满/空/回卷的用例
```

它测的是「给我一个扫描码序列,解码出的 `KeyEvent` 对不对」「buffer 满了是不是真的丢」「空的时候 poll 是不是返回 false」这些纯数据变换。注意这是一种**镜像**测法——把 `keyboard.cpp` 里的查找表和状态机抄一份到测试里测,因为内核代码本身(带 `io_inb` 内联汇编、PIC 调用)在 host 上跑不起来;机内测 `kernel/test/test_keyboard.cpp` 才会真正跑内核代码(下一节讲它怎么注入扫描码)。跑它们:

```bash
ctest --test-dir build -R keyboard --output-on-failure
```

真硬件交互(IRQ1 真能触发、handler 真能从 `0x60` 读到码)就得在 QEMU 里测了。可自动测键盘,靠的是 QEMU 的 i8042 模拟支持一条巧妙的命令:`0xD2`(write to first PS/2 port output buffer)。测试代码往 `0x64` 写 `0xD2`,再往 `0x60` 写一个扫描码,QEMU 的模拟控制器就会把这个字节放进它的输出缓冲,handler 下一次 `io_inb(0x60)` 就会读到它——等于「假装你按了这个键」:

```cpp
void inject_scancode(uint8_t sc) {
    // 往 0x64 写 0xD2: 把下一字节塞进第一口输出缓冲
    io_outb(0x64, 0xD2);
    // 往 0x60 写扫描码: 下次 io_inb(0x60) 就读到它
    io_outb(0x60, sc);
}
// 然后: inject(0x1E); Keyboard::irq1_handler(nullptr);
//       Keyboard::poll(ev);  断言 ev.ascii == 'a'
```

于是 [test_keyboard.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_keyboard.cpp)(机内测)就能自动验证:注入 `0x1E` 后 `poll` 出来 `ascii=='a'`、注入 `0x9E`(break)出来 `pressed==false`、连续注入多个码按 FIFO 顺序出来、Shift 状态正确翻转。跑它用带测试钩子的内核:

```bash
cmake --build build --target run-big-kernel-test
```

这套「注入扫描码」的测法,把「键盘交互」这个看似只能手测的东西,变成了可自动回归的测试,是这一章工程上最值钱的一笔。

## 下一站

到这里,内核第一次「有问有答」了:你敲键盘,它回显;屏幕和串口同步显示。第一个真正的输入设备接上了,中断体系第一次为一个外部设备好好干了活,「ISR 生产 + 主循环消费」的环形缓冲区也立起来了。这些是后面 shell、用户态交互的地基。

但你会察觉到一个明显的短板:这台机器至今**没有内存管理**。所有东西都跑在 bootloader 给的那套固定页表上,内核想分配一页内存、想给将来的进程分独立的地址空间,都没有机制——我们连「哪些物理页可用、哪些被占了」都还没记录。键盘、屏幕这些外设都点亮了,可内核最该管的「内存」,还是一片没开垦的荒地。

下一站,我们就动这块。要给物理内存建立账本,记录每一个页框的占用状态,提供一个「给我分配一页」「我还你一页」的接口。那是物理内存管理器(PMM)的活,也是整个内存子系统的第一块基石。不过那是下一章的事了,我们先享受一下「敲键能回显」这个小里程碑。

---

### 参考

- OSDev — [PS/2 Keyboard](https://wiki.osdev.org/PS/2_Keyboard):scan code set 1 的 make/break 编码(bit7 区分)、`0x60` 数据口与 `0x64` 状态/命令口、set 1 与 set 2 的差异。本章的扫描码解码以此为准。
- OSDev — [8042 PS/2 Controller](https://wiki.osdev.org/I8042_PS/2_Controller):控制器 config 字节各位含义(bit0 第一口中断、bit1 第二口中断、bit6 set2→set1 翻译)、self-test 命令 `0xAA` 期待 `0x55`、命令 `0xD2`(写第一口输出缓冲,本章测试用它注入扫描码)、初始化仪式。
- 011 章 · [big kernel PIC/PIT](../004/):PIC remap、IRQ0-15 → vector 0x20-0x2F、EOI 由 C handler 发送的规矩。本章 IRQ1 接线建立在那套中断体系之上。
- 本 tag 源码:[keyboard.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/keyboard/keyboard.hpp) / [keyboard.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/keyboard/keyboard.cpp)、[interrupts.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/interrupts.S)(`irq1_stub` 改接)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp)(Step 10-12 回显循环)、[pit.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/pit/pit.cpp)(删 `[TICK]` 噪声);测试 [test_keyboard.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_keyboard.cpp)(host 镜像)、[test_keyboard.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_keyboard.cpp)(QEMU `0xD2` 注入)。
