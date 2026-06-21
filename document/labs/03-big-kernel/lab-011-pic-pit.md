---
title: Lab 011 · PIC 与 PIT：让内核每秒嘀嗒一次
---

# Lab 011 · PIC 与 PIT：让内核每秒嘀嗒一次

> 配套章节：[011 · PIC、IRQ 与 PIT：让内核听见时钟](../../book/03-big-kernel/011-big-kernel-pic-pit.md)。这一篇是动手版：我们不在正文里贴答案，只给目标和约束，剩下交给你。

## 实验目标

让 big kernel 第一次响应外部硬件信号。具体验收标准只有一个现象：开机后串口稳定地每秒吐一行 `[TICK] uptime: Ns`，`N` 单调递增，且内核不会因为开中断而重启。中间还要顺手证明 010 的异常网没坏——`int $3` 仍能正常返回。

说白了，这一关的及格线是「内核拥有了时间」。

## 前置条件

动手之前你得先确认几件事：你已经过了 Lab 010，GDT 加载、IDT 建好、`int $3` 能被接住并 dump 出寄存器、然后内核活着继续；你理解上一章 `ISR_NOERRCODE` 宏的栈布局，因为 IRQ 这边要原样复用它；另外 `kprintf` 能往串口打字也得心里有数，这一关所有观测都靠它。

如果你手上的内核还会在 `sti` 之后立刻三重故障重启，先回去把 010 做扎实，别在这一关和 Double Fault 死磕。

## 任务分解

整条链可以拆成四块，按顺序做完。每块都先想清楚「为什么这一步要在这一步之前」。

**第一步：把 16 条硬件中断搬出异常区。** 写一个 PIC 驱动（双芯片、级联），按 ICW1-ICW4 序列把主片 IRQ0-7 remap 到 INT 0x20 起、从片 IRQ8-15 remap 到 INT 0x28 起。提醒两件事：ICW 之间要插延时（想想用什么手段「浪费」掉约 1 微秒）；ICW3 那两个级联魔数是 PC 平台的雷打不动约定，别自作聪明改。

**第二步：给 IRQ0-15 在 IDT 里安好家。** 往 IDT 的 0x20-0x2F 这 16 个 gate 塞 handler。建议用一张表驱动，别写 16 段重复。汇编侧给每条线配一个 stub，复用 010 的无 error-code 宏。这一步要想清楚：这些 gate 用中断门还是陷阱门？为什么？

**第三步：把 PIT 接上 IRQ0。** 配 channel 0，让它以 100 Hz 嘀嗒。命令字怎么拼（选哪个 channel、先写低字节还是高字节、哪个模式适合做稳定时钟）、除数怎么从 1.193182 MHz 算出来，都得你自己对。PIT 的 IRQ0 handler 要干三件事：计 tick、每攒满一秒打一行 uptime、然后别忘了一件生死攸关的小事。

**第四步：开两道闸。** 先单独放行 IRQ0，再 `sti`，最后进一个「开着中断停机」的 idle 循环。对比一下 010 的 idle 循环是怎么写的，想想为什么这一关必须换写法。

## 接口约束

你实现出来的东西，对外应该长这样（只给职责，不给实现）：

- `PIC::init(master_offset = 0x20, slave_offset = 0x28)`：初始化并 remap；初始化完成后，真正控制开/关的是下面这几个，别指望 init 自己帮你 mask 全部。
- `PIC::send_eoi(irq)`：参数是**硬件 IRQ 号**（0-15），不是 INT 向量。从片来的中断（irq ≥ 8）要想清楚 EOI 该发给谁、发几次。
- `PIC::mask(irq)` / `PIC::unmask(irq)` / `PIC::disable_all()`：对 IMR 的位操作。
- `PIT::init(freq_hz = 100)`：只配硬件，不会自己产生中断（中断要 IRQ0 的 gate 注册了 + IRQ0 被放行了 + CPU `sti` 了才会真的来）。
- `PIT::irq0_handler(frame)`：由汇编 stub 调用，C 链接，名字不能被 C++ 改写。
- `irq_init()`：把 IRQ stub 注册进 IDT。

初始化顺序在 main 里必须固化，给你一个检查清单：串口 → GDT → IDT → PIC → 注册 IRQ → PIT → （复测异常）→ 放行 IRQ0 → sti → idle。任何一个错位，后果不是「不嘀嗒」就是「直接重启」，想清楚每一步依赖谁。

## 验证步骤

跑带测试钩子的 kernel，它会替你检查 tick 是否真的递增、mask 是否真能抑制 IRQ0：

```bash
cmake --build build --target run-big-kernel-test
```

或者直接看现象，production kernel 的串口最直观：

```bash
cmake --build build --target run
```

纯逻辑（端口常量、ICW 各位、EOI 该发给谁、除数算得对不对）可以不依赖 QEMU 直接测：

```bash
ctest --test-dir build -R 'pic|pit' --output-on-failure
```

## 常见故障

- **只打出第一行 `uptime: 1s` 然后时间冻住。** 九成是 IRQ0 handler 里漏了那件「生死攸关的小事」（给 PIC 的回执）。回头看看 PIC 锁住一条 IRQ 后会怎样。
- **`sti` 之后立刻三重故障重启。** 往两个方向查：是不是 PIC 压根没 remap，IRQ0 以 INT 0x08 的身份撞进了 #DF 的向量？或者 IRQ0 的 gate 还没注册进 IDT 就 sti 了？
- **完全不嘀嗒，但不重启。** 闸没开全。PIC 的 IMR 和 CPU 的 IF 是两道闸，少开任何一个 IRQ0 都到不了 handler。检查你是不是只 `sti` 忘了 `unmask`，或反之。
- **`int $3` 复测时崩了，但 010 的时候是好的。** 新装的 IRQ gate 抢占了 IDT 里异常的位置，或者注册时向量号写错覆盖了异常 gate。对照 0x00-0x1F 是异常、0x20-0x2F 是 IRQ 这张表。
- **从片相关的中断（哪怕只是 default handler 那条线）来了就卡死。** 想想级联 EOI 的规则，default handler 只发主片 EOI 的写法在从片上够不够。

## 通过标准

1. 串口连续输出至少三行 `[TICK] uptime: Ns`，且 `N` 严格递增、永不停滞。
2. `int $3` 在 PIC/IRQ 体系装好之后仍能正常返回（看到 `Breakpoint returned, continuing.`）。
3. `run-big-kernel-test` 的 tick 递增、uptime 单调、mask 抑制 IRQ0 这几条行为测试全绿。
4. idle 循环是 `hlt` 而非 `cli; hlt`——CPU 处于「开着中断睡眠、被 IRQ0 唤醒」的状态。

达成这四条，你的内核就第一次「听见了时钟」。下一关，我们要让它听见从串口敲进来的每一个字符。
