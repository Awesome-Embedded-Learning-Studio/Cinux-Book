---
title: 05 · 收尾:验证与下一站
---

# 收尾:验证与下一站

## 验证

009 没有新增单测(它是一次结构性重构,验证靠的是「真的启动起来、跑通整条链路」)。

```bash
cmake --build build
make run          # 启动生产内核（挂 AHCI 盘 + ext2 盘）
```

串口里应当看到新的 init 线程路径,而且 ext2 挂载**成功**(碰撞已修):

```text
[BIG] ===== Scheduler & Init Thread =====
[INIT] kernel_init started tid=...
[INIT] ===== Milestone 028: ext2 Filesystem =====
[AHCI] ...                                          ← 不再有 command timeout
[INIT] ===== Milestone 027: VFS =====
[VFS] ext2 mounted at /
[INIT] ===== Milestone 023: Syscall from Ring 3 =====
... shell 起来
```

**关键的回归验证点**:在 init 线程里 Port 1 的 `SSTS` 应当**保持 `0x113`**,不再是 `0x0`——这直接证明「栈盖 MMIO」的碰撞没了。如果它又掉成 `0x0`,说明你又把栈基址写回了 `0x...100000`,或者布局表里区段算错了。

测试内核照常可跑(`make run-kernel-test`),它不含这次重构的 init 路径,但 008 那套 RAII 同步测试还在。

> 顺带:008 那个临时搭的 `run_concurrent_stress()` 框架(`kernel/stress/stress_test.cpp`)在 009 整个删掉了——它兼着的「起 shell」职责已交还给 `kernel_init`。别把它和 `cmake/qemu.cmake` 里那个 `run-stress-test` 目标搞混:后者是用 1 GB 合成 ELF 压测 mini-loader 装载大内核的另一条测试线,跟这次重构无关、009 里照常可用。至于原 stress 框架承担的并发验证,现在由生产启动 `make run` 本身覆盖。

## 下一站

009 让启动路径成了可调度的 init 线程,顺手把散落的内核虚拟地址收拢成一张布局表。到此,内核这边「能调度、能阻塞、地址不打架」都齐了,帧缓冲也早就点亮了(013)。下一章(029)要做的,是**在帧缓冲上画东西**——从「能往屏幕打字」走向「能在屏幕上画图形」。有了能阻塞、能让出的 init 线程,绘图循环、刷新时序这些也才有了干净的落点。具体怎么画,那是 029 的事。

---

**参考**

- Linux 内核的 idle/init 线程模型(PID 0 idle、PID 1 init 接管用户态启动):组织模型来自内核源码 `init/main.c` 的 `rest_init()`——它派生 `kernel_init`(PID 1)与 idle(PID 0),前者接管后续启动、后者进空闲循环。本章只借这个**模型**(boot 当 handoff 源 + init 线程接管),并非实现 init 全套能力。<https://github.com/torvalds/linux/blob/master/init/main.c>
- AHCI 的 ABAR / BAR5(HBA 寄存器块的 MMIO 基址):`MMIO_VIRT_BASE` 映射的就是 AHCI 的 BAR5;「读 MMIO 寄存器得全零 = 页表映射被覆盖」的依据。OSDev AHCI:<https://wiki.osdev.org/AHCI>
- placement new 的语法语义:`launch_first_user` 在对齐的静态 buffer 上构造 `AddressSpace` 所用。<https://en.cppreference.com/w/cpp/language/new>。至于「静态对象因带析构器会触发 `__dso_handle`/`__cxa_atexit` 登记、在 freestanding 下链接报错」这一层,属内核 freestanding 链接的常见经验(Itanium C++ ABI 的析构器登记机制),并非来自该页。
- x86-64 高半直接映射(`0xFFFF8000_00000000+` 的 canonical 高半区):内核虚拟内存布局落在该区间;区段化的 `memory_layout.hpp` 的地址依据。AMD64 APM / Intel SDM Vol 1(虚拟地址与 canonical 地址)。
