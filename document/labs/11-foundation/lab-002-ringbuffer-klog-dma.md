---
title: Lab 002 · RingBuffer / dmesg / DMA 池 验证
---

# Lab 002 · RingBuffer / dmesg / DMA 池 验证

> 对应 `document/book/11-foundation/002/`。验证档 **B 档**(基建/重构)+ 一点 A(`dmesg` 用户态读得到)。验证靠构建 + 测试 + grep + 看 dmesg 输出。

## 目标

确认四件事:

1. pipe 和键盘都换上了 Cinux-Base 的 `RingBuffer`,外层语义没破;
2. `ConcurrentRingBuffer` 在 `kernel/lib/`(不在 Cinux-Base),`KernelLog` + `sys_dmesg` 闭环;
3. `DmaBuffer/DmaPool/PrdtBuilder` 三个类型在,direct-map 不 unmap;
4. run-kernel-test 从 001 的 662 涨到 694(DMA +20)。

## 步骤

### 1. 构建 + 内核测试绿

```bash
git checkout 002_ringbuffer_klog_dma 2>/dev/null || git checkout 002_*
cmake -B build -S . && cmake --build build -j$(nproc)
cmake --build build --target run-kernel-test
```

**期望**:`0 error`;`run-kernel-test` 全绿,总数应在 694 上下(比 001 多 ~20,DMA 那批)。

### 2. RingBuffer 归一

```bash
grep -rn 'cinux::lib::RingBuffer' kernel/ipc/ kernel/drivers/keyboard/
# 期望:pipe.hpp:168、keyboard.hpp:112 各一处,且没有手写 buffer_/head_/tail_
grep -c 'head_\|tail_\|count_' kernel/ipc/pipe.cpp
# 期望:大幅减少(手写环形缓冲的痕迹被 push_batch/pop_batch 取代)
```

**思考**:为什么 pipe 的 `RingBuffer<char, PIPE_BUFFER_SIZE>` 用 `count_` 策略而不是牺牲槽位?——因为 Cinux-Base 的 `RingBuffer` 统一用 `count_`,正好和 pipe 原来的手写实现一致,迁移零语义损失。键盘那边原来牺牲槽位(实占 63),换过来后容量变成字面 64——这笔"小账"为什么没引发测试失败?因为没测试依赖精确容量。

### 3. dmesg 闭环

```bash
grep -rn 'class ConcurrentRingBuffer\|class KernelLog\|sys_dmesg' kernel/lib/ kernel/syscall/sys_dmesg.hpp
# 期望:concurrent_ring_buffer.hpp:40、KernelLog 类、sys_dmesg.hpp:28 签名
grep -rn 'irq_guard' kernel/lib/concurrent_ring_buffer.hpp
# 期望:每个操作都过 irq_guard——这就是它必须在 kernel/lib 而非 Cinux-Base 的原因
```

观察 dmesg 输出:跑 `run-kernel-test` 时串口/日志里找 `[INFO]`/`[WARN]` 开头、带 tick 数的行——那就是 `KernelLog` 格式化出来的历史。

### 4. DMA 三件套 + direct-map 不 unmap

```bash
ls kernel/drivers/dma/
# 期望:dma_buffer.hpp、dma_pool.hpp、dma_pool.cpp、prdt_builder.hpp
grep -n 'KERNEL_VMA\|unmap\|free_pages' kernel/drivers/dma/dma_pool.cpp
# 期望:看到 virt = phys + KERNEL_VMA 的复用;free 路径只 free_pages(phys),不 unmap
```

**思考**:为什么 `DmaPool::free` 不 `vmm.unmap(virt)`?——见章节「踩坑」。direct-map 的 PTE 是永久对照表,unmap 它会拆掉永久槽,后续 demand paging 反复映射错 phys 死循环。**RAII 的直觉在这里是错的。**

### 5.(可选)笔者视角:体会 MPSC 封装的时机

`ConcurrentRingBuffer` 为什么不放在 Cinux-Base、也不在 F1-M1 就抽?去 `kernel/lib/concurrent_ring_buffer.hpp` 看它对 `Spinlock::irq_guard` 的依赖,再回想 F1-M1 的 pipe/keyboard 都是 SPSC——你应该能自己回答:**抽通用封装的时机是"第二个真实用户出现",不是"看起来能抽象"。**

## 验收清单

- [ ] 构建 `0 error`,`run-kernel-test` 全绿(总数 ~694)。
- [ ] pipe/keyboard 都用 `cinux::lib::RingBuffer`,无手写环形缓冲残留。
- [ ] `sys_dmesg`(103)+ `KernelLog` + `ConcurrentRingBuffer`(在 `kernel/lib/`)闭环。
- [ ] DMA 三类型在;`dma_pool.cpp` 的 free 不 unmap。
- [ ] 能说清「为什么 ConcurrentRingBuffer 在 kernel/lib 不在 Cinux-Base」「为什么 DmaPool 不 unmap direct-map」。

## 别做这些

- **别**把 `ConcurrentRingBuffer` 提到 Cinux-Base——它依赖 `Spinlock`,而 Cinux-Base 是 freestanding。
- **别**在 `DmaPool::free` 里加 `vmm.unmap`——那是 GOTCHA #7,QEMU 会卡死。
