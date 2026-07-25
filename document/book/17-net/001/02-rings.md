---
title: 02 · 收发环、轮询坑与 CMake gate
---

# 收发环、轮询坑与 CMake gate

## 收发环:legacy 描述符 + DMA 缓冲

MAC 拿到了、链路拉起来了(`CTRL.SLU`),接下来立收发环。e1000 的「legacy」模式用一个定宽的描述符(16 字节)描述一个包缓冲:描述符里写「缓冲的 DMA 地址 + 长度 + 状态位」,设备和驱动各维护一个 head/tail 指针在环上推进。

- **RX 环**:32 个描述符,每个挂一个 2KB 的接收缓冲(从 DmaPool 分配)。设备收到包就往 head 指的缓冲里 DMA 写,置「Descriptor Done」位,推进 head。
- **TX 环**:8 个描述符,发完一个复用一个缓冲。

收包过滤也得设:把「自己这个 MAC」写进 Receive Address 寄存器(`RAL0`/`RAH0`),让设备收发给自己 MAC 的**单播**帧;`RCTL` 里置 `BAM` 位让它也收**广播**帧(ARP/DHCP 这些要用广播)。这两个一开,设备就会把线上匹配的包 DMA 进 RX 环。

## 轮询收包那个坑:得读一个 MMIO 寄存器

这是这一章最该记的一笔。立好环之后,最直白的收包写法是:轮询内存里下一个描述符的 status 位,看到 DD(Descriptor Done)就说明这个槽有包了,拷出来。**但在 QEMU 上,这么写永远收不到包。**

现象很气人:包明明发出去了(filter-dump pcap 证明 QEMU 的 SLIRP 网关也回了),但 ring 里一个包都没有——描述符的 status 没置,head 指针没动,收包计数寄存器 `GPRC` 是 0。环的所有配置寄存器回读全对(RX 基址、长度、控制位、描述符的缓冲地址都没问题)。

根因在 QEMU 的工作方式上,`poll_rx` 开头那条注释写得很清楚:

```cpp
bool E1000Controller::poll_rx(uint8_t* dst, uint32_t max_len, uint32_t& out_len) {
    // Read RDH (MMIO).  On real HW the packet lands in the ring the instant it
    // arrives.  Under QEMU TCG, pulling the SLIRP reply into the ring needs the
    // device-model main loop to run, which a polled ring alone does not drive
    // -- the caller (test_e1000.cpp) sti+hlt's between polls so the main loop
    // runs and a LAPIC-timer IRQ wakes it.  In production the PIT/LAPIC tick
    // already keeps the main loop alive, so this single read is plenty.
    // Harmless on real HW.
    (void)reg_read(e1000reg::RDH);
    ...
```

（`e1000.cpp:237`,关键那行 `(void)reg_read(RDH)` 在 `:245`。）真机上,包一到就被 DMA 进 ring,描述符 status 立刻置位——读不读 RDH 无所谓。**QEMU TCG 不一样**:把 SLIRP 网关的回包「拉进 ring」这一步,是设备模型的**主循环**干的;而这个主循环只在 guest **trap 出来**(MMIO 访问、中断、hlt)的时候才跑。描述符的 status 在 DMA 内存里——读它**不 trap**——所以光在内存里轮询 status,模拟器的主循环压根没跑,包就永远悬在线上、没被推进 ring。

修法就是开头那一行 `reg_read(RDH)`:读 RDH 这个 **MMIO 寄存器**(不是内存)会 trap 出去,模拟器借此跑一轮主循环,把包投递进 ring。配合调用方(测试)在两次轮询之间 `sti; hlt`(让 LAPIC 定时器中断把 CPU 唤醒、再多 trap 几次),包就到了。生产环境里 PIT/LAPIC 的 tick 本来就在不停打断 CPU,主循环一直活着,这一行读就够。

> **这不是 QEMU 的 bug,是轮询驱动在模拟器上的正确交互方式。** 定位它的时候最容易走偏的是:看到「ring 寄存器全对、包却没来」,去怀疑配置(是不是过滤没开、是不是缓冲地址错了)。但收包计数 `GPRC`(一个 MMIO 统计寄存器)能区分两种情况——`GPRC=0` 说明 MAC 压根没把包收进设备(过滤问题),`GPRC>0` 但 ring 里没有说明收了但没 DMA 到 ring(投递问题)。这里是 `GPRC=0` 且 ring 寄存器全对,指向「投递没发生」→ 模拟器主循环没跑 → 内存轮询没 trap。**用诊断寄存器把「没收」和「收了没送」分开**,是定位这类问题的钥匙,别一上来就归因到最显眼的共同路径(配置)上。

## CMake 文件 gate:又见 §14

这个驱动也走 054b 立的那条规矩:源码里不写 `#ifdef`,改用 CMake 的文件级 gate。`kernel/drivers/CMakeLists.txt` 里:

```cmake
if(CINUX_NET)
    net/e1000.cpp
    net/e1000_init.cpp
else()
    net/net_stub.cpp      # CINUX_NET off 时编一个空 net::init()
endif()
```

`CINUX_NET` 是 `ON` 时编真驱动,`OFF` 时编一个 `net_stub.cpp`——里头是个空的 `net::init()`,让 `kernel/main.cpp` 里那句 `net::init()` 调用照样链接得过(`main.cpp` 里**一行 `#ifdef` 都没有**)。这和 054b 把 `#ifdef CINUX_GUI` 赶去 CMake 是同一个手法:调用处零 `#ifdef`,开关只在构建系统里管「编哪份文件」。测试侧也一样:`test_e1000.cpp` 挂在 `if(CINUX_NET)` 下,`main_test.cpp` 里 `run_e1000_tests()` 的声明和调用挂 `#ifdef CINUX_NET`——比 xHCI 当年没守卫的 test 还干净一档(那个是 054c 才补上的)。
